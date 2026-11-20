#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The visual
// editor of motion (REMOTION-PARITY-2026-09 REM5): the selection drawn over
// Record, its gestures, its context menu and the Inspector of a node.
// MotionCanvas.h decides; these methods relay. Moved out of main.mm
// unchanged.

@interface AppDelegate (Motion) <CMMotionInspectorViewDelegate>
- (std::string)motionMediaDirectory;
- (BOOL)motionCanvasPrepare;
- (BOOL)motionCanvasContainsEvent:(NSEvent*)event;
- (void)motionCanvasPoint:(NSEvent*)event x:(double&)x y:(double&)y;
- (std::vector<std::pair<double, double>>)motionCanvasViewQuad:
    (const std::pair<double, double>*)quad;
- (void)motionCanvasRedraw;
- (void)motionCanvasShowInspector;
- (void)motionInspector:(CMMotionInspectorView*)view selectNode:(NSString*)uid;
- (void)motionCanvasRefresh;
- (void)motionCanvasSelect:(const std::string&)uid;
- (void)motionCanvasHover:(NSEvent*)event;
- (void)motionCanvasMouseDown:(NSEvent*)event;
- (void)motionCanvasMouseDragged:(NSEvent*)event;
- (void)motionCanvasMouseUp:(NSEvent*)event;
- (BOOL)motionCanvasCommit:(const mcp_json::Value&)definition
                     label:(NSString*)label;
- (BOOL)motionCanvasApply:(Operation)operation label:(NSString*)label;
- (BOOL)motionCanvasKeyDown:(NSEvent*)event;
- (void)motionCanvasRemoveSelected:(id)sender;
- (NSMenuItem*)motionMenuItem:(NSString*)title action:(SEL)action;
- (std::string)motionCanvasParentUid;
- (BOOL)motionCanvasSeriesOf:(const std::string&)uid
                      series:(std::string&)series
                        next:(const mcp_json::Value*&)next;
- (NSMenu*)motionCanvasMenuForEvent:(NSEvent*)event;
- (void)motionCanvasInsert:(const mcp_json::Value&)node label:(NSString*)label;
- (void)motionCanvasAddText:(id)sender;
- (void)motionCanvasAddBox:(id)sender;
- (void)motionCanvasAddVideo:(id)sender;
- (void)motionCanvasAddImage:(id)sender;
- (void)motionCanvasAddScene:(id)sender;
- (void)motionCanvasAddTransition:(NSMenuItem*)sender;
- (void)motionCanvasSelectTransition:(id)sender;
- (void)motionCanvasCreate:(BOOL)scenes;
- (void)motionCanvasNewComposition:(id)sender;
- (void)motionCanvasNewScenes:(id)sender;
- (const MotionField*)motionCanvasField:(NSString*)property;
- (void)motionInspector:(CMMotionInspectorView*)view
            commitField:(NSString*)property
                   text:(NSString*)text;
- (void)motionInspector:(CMMotionInspectorView*)view
              toggleKey:(NSString*)property;
- (void)motionInspector:(CMMotionInspectorView*)view
                 easing:(NSString*)easing
            forProperty:(NSString*)property;
- (void)motionInspector:(CMMotionInspectorView*)view
                 spring:(NSString*)property
                     on:(BOOL)on
                   from:(double)from
                     to:(double)to
                  delay:(NSInteger)delay
                damping:(double)damping;
- (void)motionInspectorDidClose:(CMMotionInspectorView*)view;
@end
