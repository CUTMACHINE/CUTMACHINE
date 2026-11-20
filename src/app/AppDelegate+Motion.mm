// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The visual
// editor of motion (REMOTION-PARITY-2026-09 REM5): the selection drawn over
// Record, its gestures, its context menu and the Inspector of a node.
// MotionCanvas.h decides; these methods relay. Moved out of main.mm
// unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (Motion)

- (std::string)motionMediaDirectory {
    return self.documentPath.length > 0
               ? std::filesystem::absolute(self.documentPath.UTF8String)
                     .parent_path()
                     .string()
               : std::string();
}

// The motion clip Record shows at the playhead -- the topmost one -- its
// frame evaluated, and where its pixels sit in the monitor.
- (BOOL)motionCanvasPrepare {
    MotionCanvasState& canvas = self.state->motionCanvas;
    canvas.has_frame = false;
    const DocumentClip* clip = nullptr;
    for (auto slot = self.state->requested.rbegin();
         slot != self.state->requested.rend(); ++slot) {
        if (!slot->active) continue;
        const DocumentClip* candidate =
            self.state->document.FindClip(slot->clipId);
        if (candidate && IsMotionClip(*candidate)) {
            clip = candidate;
            break;
        }
    }
    if (!clip) return NO;
    MotionClipInstant instant;
    std::string errorName;
    std::string message;
    MotionReach reach;
    MotionError error;
    MotionComposition resolved;
    mcp_json::Value props;
    if (!MotionClipAt(self.state->document, clip->id,
                      self.state->requestedPosition, instant, errorName,
                      message) ||
        !MotionReachFor(self.state->document, instant.composition->id, reach,
                        error) ||
        !ResolveMotionVariant(instant.parsed, std::string(), std::string(),
                              clip->motion_props, resolved, props, error))
        return NO;
    MotionCoreTextMeasurer measurer([self motionMediaDirectory]);
    measurer.SetMediaPaths(reach.media_paths);
    MotionFrame frame;
    if (!EvaluateMotionFrame(resolved, props, instant.frame, measurer,
                             reach.Context(), frame, error))
        return NO;
    // MA6 -- the layers, named as After names them.
    const std::string outlineKey = instant.composition->id + "\n" +
                                   props.Dump() + "\n" +
                                   instant.composition->definition.Dump();
    if (canvas.outline_key != outlineKey) {
        MotionError outlineError;
        if (!MotionOutlineRows(resolved, props, measurer, reach.Context(),
                               canvas.outline, outlineError)) {
            canvas.outline.clear();
        }
        canvas.outline_key = outlineKey;
    }
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    ClipFramingGeometry framing;
    if (!ResolveClipFraming(clip->framing, resolved.width, resolved.height,
                            sequenceWidth, sequenceHeight, framing, message))
        return NO;
    // The frame's canvas in the monitor, as the captions are placed on it.
    const NSRect bounds = self.programMonitorView.bounds;
    const double layerScale = self.programMonitorView.layer.contentsScale;
    const double zoomPoints =
        self.state->programMonitorZoom > 0.0 && layerScale > 0.0
            ? self.state->programMonitorZoom / layerScale
            : 0.0;
    const ui::caption_overlay::Layout layout = ui::caption_overlay::Compute(
        sequenceWidth, sequenceHeight, bounds.size.width, bounds.size.height,
        zoomPoints);
    if (canvas.composition_id != instant.composition->id) {
        canvas.selected.clear();
        canvas.hovered.clear();
    }
    canvas.clip_id = clip->id;
    canvas.composition_id = instant.composition->id;
    canvas.composition_frame = instant.frame;
    canvas.frame = std::move(frame);
    canvas.mapping = MotionMonitorMappingFor(framing, layout.canvas_x,
                                             layout.canvas_y, layout.scale);
    canvas.has_frame = true;
    return YES;
}

- (BOOL)motionCanvasContainsEvent:(NSEvent*)event {
    if (!self.programMonitorView ||
        event.window != self.programMonitorView.window)
        return NO;
    const NSPoint point =
        [self.programMonitorView convertPoint:event.locationInWindow
                                     fromView:nil];
    return NSPointInRect(point, self.programMonitorView.bounds);
}

// The event in the composition's pixels.
- (void)motionCanvasPoint:(NSEvent*)event x:(double&)x y:(double&)y {
    const NSPoint point =
        [self.programMonitorView convertPoint:event.locationInWindow
                                     fromView:nil];
    self.state->motionCanvas.mapping.FromView(
        point.x, self.programMonitorView.bounds.size.height - point.y, x, y);
}

- (std::vector<std::pair<double, double>>)motionCanvasViewQuad:
    (const std::pair<double, double>*)quad {
    std::vector<std::pair<double, double>> points;
    for (int corner = 0; corner < 4; ++corner) {
        double x = 0.0;
        double y = 0.0;
        self.state->motionCanvas.mapping.ToView(quad[corner].first,
                                                quad[corner].second, x, y);
        points.emplace_back(x, y);
    }
    return points;
}

// What the overlay draws: the node under the mouse, the one selected and
// its handles -- moved by the gesture in progress, which is only a preview.
- (void)motionCanvasRedraw {
    const MotionCanvasState& canvas = self.state->motionCanvas;
    std::vector<std::pair<double, double>> hover;
    std::vector<std::pair<double, double>> selection;
    std::vector<std::pair<double, double>> handles;
    // Nothing over the picture in full screen. The selection is kept and
    // comes back with the window (toggleFullscreen).
    if (self.videoFullscreenActive) {
        [self.motionCanvasView setHover:hover
                              selection:selection
                                handles:handles];
        return;
    }
    std::pair<double, double> quad[4];
    if (canvas.has_frame && !canvas.hovered.empty() &&
        canvas.hovered != canvas.selected &&
        MotionNodeQuad(canvas.frame, canvas.hovered, quad))
        hover = [self motionCanvasViewQuad:quad];
    if (canvas.has_frame && !canvas.selected.empty() &&
        MotionNodeQuad(canvas.frame, canvas.selected, quad)) {
        if (canvas.gesture == 1) {
            for (auto& corner : quad) {
                corner.first += canvas.delta_x;
                corner.second += canvas.delta_y;
            }
        } else if (canvas.gesture == 2) {
            // The corner or the edge dragged moves; the rest stays.
            static const int kCorners[8][2] = {{0, 0}, {1, 1}, {2, 2}, {3, 3},
                                               {0, 1}, {1, 2}, {2, 3}, {3, 0}};
            for (const int corner : kCorners[canvas.handle]) {
                quad[corner].first += canvas.delta_x;
                quad[corner].second += canvas.delta_y;
            }
        }
        selection = [self motionCanvasViewQuad:quad];
        std::pair<double, double> viewQuad[4];
        for (int corner = 0; corner < 4; ++corner)
            viewQuad[corner] = selection[static_cast<size_t>(corner)];
        handles = MotionSelectionHandles(viewQuad);
    }
    [self.motionCanvasView setHover:hover selection:selection handles:handles];
}

// The Inspector shows the node selected, or the clip when there is none.
// MA6 -- the composition's layers above its fields; a layer chosen in the
// list that this frame does not show keeps the list, without fields.
- (void)motionCanvasShowInspector {
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    std::string error;
    if (!canvas.selected.empty() && stored != nullptr && canvas.has_frame) {
        const bool shown =
            MotionNodeFields(stored->definition, canvas.frame, canvas.selected,
                             canvas.composition_frame, canvas.fields, error);
        if (!shown) {
            canvas.fields.clear();
        }
        MotionNodeLocation location;
        std::string title = "Motion";
        if (FindMotionNode(stored->definition, canvas.selected, location)) {
            const mcp_json::Value* node =
                MotionTreeAt(stored->definition, location.path);
            const mcp_json::Value* type = node ? node->Find("type") : nullptr;
            const mcp_json::Value* name = node ? node->Find("id") : nullptr;
            title =
                stored->name + " · " +
                (type && type->IsString() ? type->AsString() : "") +
                (name && name->IsString() ? " " + name->AsString() : "") +
                (shown ? " · image " + std::to_string(canvas.composition_frame)
                       : " · pas montré à cette image");
        }
        [self.motionInspectorView
            showTitle:CMStringFromUtf8(title)
              outline:canvas.outline
                shown:MotionOutlineShown(canvas.outline, canvas.selected)
             selected:canvas.selected
               fields:canvas.fields];
        if (!self.motionInspectorShown) {
            [self.rightDockPanel setContentView:self.motionInspectorView
                                        forSlot:ui::PanelSlot::Inspector];
            self.motionInspectorShown = YES;
        }
        return;
    }
    canvas.fields.clear();
    if (self.motionInspectorShown) {
        [self.rightDockPanel setContentView:self.inspectorView
                                    forSlot:ui::PanelSlot::Inspector];
        self.motionInspectorShown = NO;
    }
}

- (void)motionInspector:(CMMotionInspectorView*)view selectNode:(NSString*)uid {
    (void)view;
    [self motionCanvasSelect:uid.UTF8String];
}

- (void)motionCanvasRefresh {
    if (![self motionCanvasPrepare]) {
        self.state->motionCanvas.hovered.clear();
    }
    [self motionCanvasRedraw];
    [self motionCanvasShowInspector];
}

- (void)motionCanvasSelect:(const std::string&)uid {
    self.state->motionCanvas.selected = uid;
    [self motionCanvasRedraw];
    [self motionCanvasShowInspector];
}

- (void)motionCanvasHover:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (!canvas.has_frame && ![self motionCanvasPrepare]) {
        if (!canvas.hovered.empty()) {
            canvas.hovered.clear();
            [self motionCanvasRedraw];
        }
        return;
    }
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    MotionHit hit;
    const std::string hovered =
        MotionHitTest(canvas.frame, x, y, false, hit) ? hit.uid : std::string();
    if (hovered != canvas.hovered) {
        canvas.hovered = hovered;
        [self motionCanvasRedraw];
    }
}

- (void)motionCanvasMouseDown:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    canvas.gesture = 0;
    if (![self motionCanvasPrepare]) {
        [self motionCanvasSelect:std::string()];
        return;
    }
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    canvas.start_x = x;
    canvas.start_y = y;
    canvas.delta_x = canvas.delta_y = 0.0;
    // A handle of the selection, within six points, resizes it.
    std::pair<double, double> quad[4];
    if (!canvas.selected.empty() &&
        MotionNodeQuad(canvas.frame, canvas.selected, quad)) {
        const auto handles = MotionSelectionHandles(quad);
        for (size_t index = 0; index < handles.size(); ++index) {
            double hx = 0.0;
            double hy = 0.0;
            double px = 0.0;
            double py = 0.0;
            canvas.mapping.ToView(handles[index].first, handles[index].second,
                                  hx, hy);
            canvas.mapping.ToView(x, y, px, py);
            if (std::hypot(hx - px, hy - py) <= 6.0) {
                canvas.gesture = 2;
                canvas.handle = static_cast<int>(index);
                return;
            }
        }
    }
    MotionHit hit;
    if (MotionHitTest(canvas.frame, x, y, false, hit)) {
        [self motionCanvasSelect:hit.uid];
        canvas.gesture = 1;
    } else {
        [self motionCanvasSelect:std::string()];
    }
}

- (void)motionCanvasMouseDragged:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    canvas.delta_x = x - canvas.start_x;
    canvas.delta_y = y - canvas.start_y;
    [self motionCanvasRedraw];
}

- (void)motionCanvasMouseUp:(NSEvent*)event {
    (void)event;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const int gesture = canvas.gesture;
    canvas.gesture = 0;
    // Less than half a pixel of the composition: a click, not a drag.
    if (std::hypot(canvas.delta_x, canvas.delta_y) < 0.5 ||
        canvas.selected.empty()) {
        [self motionCanvasRedraw];
        return;
    }
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return;
    std::vector<MotionPropertyEdit> edits;
    std::string error;
    const bool built =
        gesture == 1
            ? MotionMoveEdits(canvas.frame, stored->definition, canvas.selected,
                              canvas.composition_frame, canvas.delta_x,
                              canvas.delta_y, edits, error)
            : MotionResizeEdits(canvas.frame, stored->definition,
                                canvas.selected, canvas.composition_frame,
                                canvas.handle, canvas.delta_x, canvas.delta_y,
                                edits, error);
    mcp_json::Value definition;
    if (!built || !MotionDefinitionWithEdits(stored->definition, edits,
                                             definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        canvas.delta_x = canvas.delta_y = 0.0;
        [self motionCanvasRedraw];
        return;
    }
    canvas.delta_x = canvas.delta_y = 0.0;
    [self motionCanvasCommit:definition
                       label:gesture == 1 ? @"Motion : déplacé"
                                          : @"Motion : redimensionné"];
}

// One gesture, one operation: the whole new definition, which one undo
// takes back.
- (BOOL)motionCanvasCommit:(const mcp_json::Value&)definition
                     label:(NSString*)label {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    if (!stored) return NO;
    DocumentComposition updated = *stored;
    updated.definition = definition;
    return [self motionCanvasApply:Operation {
        UpdateMotionCompositionOperation { updated }
    }
                             label:label];
}

- (BOOL)motionCanvasApply:(Operation)operation label:(NSString*)label {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    [self motionCanvasRefresh];
    self.infoLabel.stringValue = label;
    return YES;
}

- (BOOL)motionCanvasKeyDown:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (event.keyCode == 53) {  // Escape
        [self motionCanvasSelect:std::string()];
        return YES;
    }
    if (event.keyCode == 51 || event.keyCode == 117) {  // Delete
        [self motionCanvasRemoveSelected:nil];
        return YES;
    }
    double dx = 0.0;
    double dy = 0.0;
    const double step =
        (event.modifierFlags & NSEventModifierFlagShift) != 0 ? 10.0 : 1.0;
    switch (event.keyCode) {
        case 123:
            dx = -step;
            break;
        case 124:
            dx = step;
            break;
        case 125:
            dy = step;
            break;
        case 126:
            dy = -step;
            break;
        default:
            return NO;
    }
    if (![self motionCanvasPrepare]) return NO;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    std::vector<MotionPropertyEdit> edits;
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        !MotionMoveEdits(canvas.frame, stored->definition, canvas.selected,
                         canvas.composition_frame, dx, dy, edits, error) ||
        !MotionDefinitionWithEdits(stored->definition, edits, definition,
                                   error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return YES;
    }
    [self motionCanvasCommit:definition label:@"Motion : déplacé"];
    return YES;
}

- (void)motionCanvasRemoveSelected:(id)sender {
    (void)sender;
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (canvas.selected.empty()) return;
    const std::string uid = canvas.selected;
    canvas.selected.clear();
    [self motionCanvasApply:Operation {
        RemoveMotionNodeOperation { canvas.composition_id, uid }
    }
                      label:@"Motion : nœud supprimé"];
}

- (NSMenuItem*)motionMenuItem:(NSString*)title action:(SEL)action {
    NSMenuItem* item = [self menuItem:title action:action key:@""];
    item.target = self;
    return item;
}

// Where a new node goes: into the node selected when it holds children,
// else into the composition's root.
- (std::string)motionCanvasParentUid {
    const MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return {};
    MotionNodeLocation location;
    if (!canvas.selected.empty() &&
        FindMotionNode(stored->definition, canvas.selected, location)) {
        const mcp_json::Value* node =
            MotionTreeAt(stored->definition, location.path);
        const mcp_json::Value* type = node ? node->Find("type") : nullptr;
        const std::string name =
            type && type->IsString() ? type->AsString() : std::string();
        if (name == "box" || name == "fill" || name == "sequence")
            return canvas.selected;
    }
    const mcp_json::Value* root = stored->definition.Find("root");
    const mcp_json::Value* uid = root ? root->Find("uid") : nullptr;
    return uid && uid->IsString() ? uid->AsString() : std::string();
}

// The series a sequence sits in, and the node that follows it there.
- (BOOL)motionCanvasSeriesOf:(const std::string&)uid
                      series:(std::string&)series
                        next:(const mcp_json::Value*&)next {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    MotionNodeLocation location;
    if (!stored || uid.empty() ||
        !FindMotionNode(stored->definition, uid, location) ||
        location.parent.empty() && !location.is_root)
        return NO;
    const mcp_json::Value* parent =
        MotionTreeAt(stored->definition, location.parent);
    const mcp_json::Value* type = parent ? parent->Find("type") : nullptr;
    if (!type || !type->IsString() || type->AsString() != "series") return NO;
    const mcp_json::Value* id = parent->Find("uid");
    series = id && id->IsString() ? id->AsString() : std::string();
    next = nullptr;
    if (location.path.empty() || !location.path.back().is_index) return YES;
    const mcp_json::Value* children = parent->Find("children");
    const size_t index = location.path.back().index;
    if (children && index + 1 < children->AsArray().size())
        next = &children->AsArray()[index + 1];
    return YES;
}

- (NSMenu*)motionCanvasMenuForEvent:(NSEvent*)event {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Motion"];
    menu.autoenablesItems = NO;
    const BOOL shown = [self motionCanvasPrepare];
    if (shown) {
        double x = 0.0;
        double y = 0.0;
        [self motionCanvasPoint:event x:x y:y];
        MotionHit hit;
        if (MotionHitTest(self.state->motionCanvas.frame, x, y, false, hit))
            [self motionCanvasSelect:hit.uid];
        [menu addItem:[self motionMenuItem:@"Ajouter un texte"
                                    action:@selector(motionCanvasAddText:)]];
        [menu addItem:[self motionMenuItem:@"Ajouter une boîte"
                                    action:@selector(motionCanvasAddBox:)]];
        NSMenuItem* video =
            [self motionMenuItem:@"Ajouter la vidéo du rush sélectionné"
                          action:@selector(motionCanvasAddVideo:)];
        video.enabled = [self selectedMediaIds].count > 0;
        [menu addItem:video];
        [menu addItem:[self motionMenuItem:@"Ajouter une image…"
                                    action:@selector(motionCanvasAddImage:)]];
        std::string series;
        const mcp_json::Value* next = nullptr;
        const std::string& selected = self.state->motionCanvas.selected;
        if ([self motionCanvasSeriesOf:selected series:series next:next]) {
            [menu addItem:NSMenuItem.separatorItem];
            [menu
                addItem:[self motionMenuItem:@"Ajouter une scène à la suite"
                                      action:@selector(motionCanvasAddScene:)]];
            const mcp_json::Value* nextType =
                next ? next->Find("type") : nullptr;
            const std::string following =
                nextType && nextType->IsString() ? nextType->AsString() : "";
            if (following == "sequence") {
                NSMenuItem* transition = [[NSMenuItem alloc]
                    initWithTitle:@"Transition vers la scène suivante"
                           action:nil
                    keyEquivalent:@""];
                NSMenu* presentations =
                    [[NSMenu alloc] initWithTitle:@"Transition"];
                NSArray* names = @[ @"Fondu", @"Glissé", @"Volet", @"Iris" ];
                for (NSUInteger index = 0; index < names.count; ++index) {
                    NSMenuItem* item = [self
                        motionMenuItem:names[index]
                                action:@selector(motionCanvasAddTransition:)];
                    item.tag = static_cast<NSInteger>(index);
                    [presentations addItem:item];
                }
                transition.submenu = presentations;
                [menu addItem:transition];
            } else if (following == "transition") {
                [menu
                    addItem:[self
                                motionMenuItem:@"Régler la transition suivante"
                                        action:@selector
                                        (motionCanvasSelectTransition:)]];
            }
        }
        if (!selected.empty()) {
            [menu addItem:NSMenuItem.separatorItem];
            [menu addItem:[self motionMenuItem:@"Supprimer le nœud"
                                        action:@selector
                                        (motionCanvasRemoveSelected:)]];
        }
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:[self motionMenuItem:@"Nouvelle composition de motion ici"
                                action:@selector(motionCanvasNewComposition:)]];
    [menu addItem:[self motionMenuItem:@"Nouvelle composition en scènes ici"
                                action:@selector(motionCanvasNewScenes:)]];
    return menu;
}

- (void)motionCanvasInsert:(const mcp_json::Value&)node label:(NSString*)label {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    const std::string parent = [self motionCanvasParentUid];
    mcp_json::Value definition;
    std::string error;
    if (!stored || parent.empty() ||
        !MotionDefinitionWithChild(stored->definition, parent, std::string(),
                                   node, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:label];
}

- (void)motionCanvasAddText:(id)sender {
    (void)sender;
    [self motionCanvasInsert:MotionNewNode("text")
                       label:@"Motion : texte ajouté"];
}

- (void)motionCanvasAddBox:(id)sender {
    (void)sender;
    [self motionCanvasInsert:MotionNewNode("box")
                       label:@"Motion : boîte ajoutée"];
}

- (void)motionCanvasAddVideo:(id)sender {
    (void)sender;
    NSString* media = [self selectedMediaIds].firstObject;
    if (!media) return;
    mcp_json::Value node = MotionNewNode("video");
    node.Set("media", mcp_json::Value::MakeString(media.UTF8String));
    [self motionCanvasInsert:node label:@"Motion : vidéo ajoutée"];
}

- (void)motionCanvasAddImage:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[ UTTypeImage ];
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    mcp_json::Value node = MotionNewNode("image");
    node.Set("src", mcp_json::Value::MakeString(panel.URL.path.UTF8String));
    [self motionCanvasInsert:node label:@"Motion : image ajoutée"];
}

- (void)motionCanvasAddScene:(id)sender {
    (void)sender;
    std::string series;
    const mcp_json::Value* next = nullptr;
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        ![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !MotionDefinitionWithScene(stored->definition, series,
                                   MotionNewNode("sequence"), 50, definition,
                                   error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : scène ajoutée"];
}

- (void)motionCanvasAddTransition:(NSMenuItem*)sender {
    static const char* kPresentations[] = {"fade", "slide", "wipe", "iris"};
    const NSInteger index = std::clamp<NSInteger>(sender.tag, 0, 3);
    std::string series;
    const mcp_json::Value* next = nullptr;
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        ![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !MotionDefinitionWithChild(
            stored->definition, series, self.state->motionCanvas.selected,
            MotionNewTransition(kPresentations[index], "from-right", 10,
                                "ease-in-out"),
            definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : transition ajoutée"];
}

- (void)motionCanvasSelectTransition:(id)sender {
    (void)sender;
    std::string series;
    const mcp_json::Value* next = nullptr;
    if (![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !next)
        return;
    const mcp_json::Value* uid = next->Find("uid");
    if (uid && uid->IsString()) [self motionCanvasSelect:uid->AsString()];
}

// A new composition at the timeline's size, cadence and five seconds,
// posed at the playhead on the lowest video track free there: one step.
- (void)motionCanvasCreate:(BOOL)scenes {
    const DocumentSequence& sequence = self.state->document.sequence;
    const MediaRate rate = sequence.frame_rate;
    const int64_t frames =
        std::max<int64_t>(1, (5 * rate.num + rate.den - 1) / rate.den);
    const RationalTime duration{frames * rate.den, rate.num};
    const RationalTime at = self.state->requestedPosition;
    const DocumentTrack* free = nullptr;
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document)) {
        if (track->kind != "video" || track->locked) continue;
        const bool clear =
            std::none_of(track->clips.begin(), track->clips.end(),
                         [&](const DocumentClip& clip) {
                             return clip.timeline_in < at.add(duration) &&
                                    at < clip.timeline_in.add(clip.duration);
                         });
        if (clear) free = track;
    }
    if (!free) {
        self.infoLabel.stringValue =
            @"Motion : aucune piste vidéo libre au playhead, ajoutez-en une";
        return;
    }
    const Ulid composition = GenerateUlid();
    AddMotionCompositionOperation add;
    add.composition = {
        composition, scenes ? "Scènes" : "Composition",
        MotionNewComposition(sequence.width, sequence.height, rate.num,
                             rate.den, frames, scenes)};
    InsertMotionClipOperation insert;
    insert.track_id = free->id;
    insert.composition_id = composition;
    insert.source_in = {0, rate.num};
    insert.duration = duration;
    insert.timeline_in = at;
    BatchOperation batch;
    batch.label = "Nouvelle composition de motion";
    batch.operations = {Operation{add}, Operation{insert}};
    [self motionCanvasApply:Operation{batch}
                      label:@"Motion : composition créée au playhead"];
}

- (void)motionCanvasNewComposition:(id)sender {
    (void)sender;
    [self motionCanvasCreate:NO];
}

- (void)motionCanvasNewScenes:(id)sender {
    (void)sender;
    [self motionCanvasCreate:YES];
}

- (const MotionField*)motionCanvasField:(NSString*)property {
    const std::string name = property.UTF8String;
    for (const MotionField& field : self.state->motionCanvas.fields)
        if (field.property == name) return &field;
    return nullptr;
}

- (void)motionInspector:(CMMotionInspectorView*)view
            commitField:(NSString*)property
                   text:(NSString*)text {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const MotionField* field = [self motionCanvasField:property];
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!field || !stored) return;
    // A field of the node itself, a text's cascade, or a style: the
    // portable policy decides (MotionCanvas.h).
    mcp_json::Value definition;
    std::string error;
    if (!MotionDefinitionWithTypedField(
            stored->definition, canvas.selected, *field, text.UTF8String,
            canvas.composition_frame, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        [self motionCanvasShowInspector];
        return;
    }
    [self motionCanvasCommit:definition
                       label:[NSString stringWithFormat:@"Motion : %@",
                                                        CMStringFromUtf8(
                                                            field->label)]];
}

- (void)motionInspector:(CMMotionInspectorView*)view
              toggleKey:(NSString*)property {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const MotionField* field = [self motionCanvasField:property];
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!field || !stored) return;
    const std::string name = property.UTF8String;
    mcp_json::Value definition;
    std::string error;
    bool built = false;
    if (field->key_here) {
        MotionNodeLocation location;
        const mcp_json::Value* node =
            FindMotionNode(stored->definition, canvas.selected, location)
                ? MotionTreeAt(stored->definition, location.path)
                : nullptr;
        const mcp_json::Value* animate = node ? node->Find("animate") : nullptr;
        const mcp_json::Value* animation =
            animate ? animate->Find(name) : nullptr;
        std::optional<mcp_json::Value> fewer;
        built =
            animation &&
            MotionAnimationWithoutKey(*animation, canvas.composition_frame,
                                      fewer, error) &&
            MotionDefinitionWithAnimation(stored->definition, canvas.selected,
                                          name, fewer, definition, error);
    } else {
        const mcp_json::Value value =
            field->kind == MotionFieldKind::Number
                ? MotionNumberValue(std::strtod(field->text.c_str(), nullptr))
                : mcp_json::Value::MakeString(field->text);
        built = MotionDefinitionWithEdits(
            stored->definition,
            {{canvas.selected, name, value, true, canvas.composition_frame}},
            definition, error);
    }
    if (!built) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition
                       label:field->key_here ? @"Motion : clé retirée"
                                             : @"Motion : clé posée"];
}

- (void)motionInspector:(CMMotionInspectorView*)view
                 easing:(NSString*)easing
            forProperty:(NSString*)property {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return;
    const std::string name = property.UTF8String;
    MotionNodeLocation location;
    const mcp_json::Value* node =
        FindMotionNode(stored->definition, canvas.selected, location)
            ? MotionTreeAt(stored->definition, location.path)
            : nullptr;
    const mcp_json::Value* animate = node ? node->Find("animate") : nullptr;
    const mcp_json::Value* animation = animate ? animate->Find(name) : nullptr;
    mcp_json::Value eased;
    mcp_json::Value definition;
    std::string error;
    if (!animation ||
        !MotionAnimationWithEasing(*animation, easing.UTF8String, eased,
                                   error) ||
        !MotionDefinitionWithAnimation(stored->definition, canvas.selected,
                                       name, eased, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : courbe"];
}

- (void)motionInspector:(CMMotionInspectorView*)view
                 spring:(NSString*)property
                     on:(BOOL)on
                   from:(double)from
                     to:(double)to
                  delay:(NSInteger)delay
                damping:(double)damping {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored || !MotionDefinitionWithSpring(
                       stored->definition, canvas.selected, property.UTF8String,
                       on, from, to, std::max<NSInteger>(0, delay),
                       damping > 0.0 ? damping : 10.0, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition
                       label:on ? @"Motion : ressort"
                                : @"Motion : ressort retiré"];
}

- (void)motionInspectorDidClose:(CMMotionInspectorView*)view {
    (void)view;
    [self motionCanvasSelect:std::string()];
}

@end
