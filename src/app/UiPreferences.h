#pragma once

// AppKit/Foundation-only. Unverified beyond a manual read -- see the F2.1
// report.
//
// PHILOSOPHY.md's amended non-buts: interface state persistence is a local
// preference, never project truth. "Panneaux déplaçables" (movable panels)
// are named explicitly as the kind of thing that may now be *remembered for
// comfort*, not the kind of thing that must exist. F2.1 does not build a
// rearrangeable layout at all (see PanelLayout.h) -- the only interface
// state this file remembers is which of the already-fixed tabs was last
// selected in a dock, and it remembers that in NSUserDefaults only.
//
// Nothing in this file ever touches Document, Project, ProjectStorage or
// any project package on disk. Re-opening the same project on a different
// machine, with a different (or absent) NSUserDefaults value here, must
// select the same tab a fresh install would: FixedPanelLayout()'s first
// entry for that dock, never something the project file remembers.

#import <Foundation/Foundation.h>

#include "AgentWindow.h"
#include "MonitorCompare.h"
#include "PanelLayout.h"
#include "WorkspacePages.h"

#include <optional>

@interface CMUiPreferences : NSObject

+ (instancetype)sharedPreferences;

// `identifier` should be a PanelSlotDescriptor.identifier from
// PanelLayout.h. Passing anything else is harmless (it round-trips) but
// pointless, since CMPanelHostView only ever asks for identifiers that are
// already in the fixed layout.
- (void)setLastActiveIdentifier:(NSString*)identifier
                        forDock:(ui::PanelDock)dock;

// Returns nil when no preference has been recorded yet (fresh install, or a
// project opened on a machine that never touched this dock before) --
// callers fall back to FixedPanelLayout()'s first slot for that dock, never
// to anything read from the project.
- (nullable NSString*)lastActiveIdentifierForDock:(ui::PanelDock)dock;
// F2.3 -- the Media panel's own tabs, inside the left dock's one slot: a
// ui::media_panel::TabIdentifier, nil until one was chosen.
- (void)setLastMediaPanelTab:(NSString* _Nonnull)identifier;
- (nullable NSString*)lastMediaPanelTab;

// B9 -- automation can opt out of implicit active-timeline state. This is a
// machine-local safety policy, never project data.
- (BOOL)requiresExplicitTimelineForAgentEdits;

// G6 -- docs/roadmap/GRADE-2026-09.md. The Record monitor's comparison
// (MonitorCompare.h): shown without the grade, split with the reference
// still, and where. Machine-local, like the scopes.
- (MonitorCompare)monitorCompare;
- (void)setMonitorCompare:(const MonitorCompare&)compare;
// The reference still's image file, in this user's Application Support; nil
// when there is none.
- (nullable NSString*)referenceStillPath;
- (void)setReferenceStillPath:(nullable NSString*)path;

// DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md (case 64). Whether a fold
// of the Inspector's grade sliders -- `group` "wheels" (« Curseurs par
// canal ») or "curves" (« Curseurs (écart à la diagonale) ») -- was left
// open. They opened folded at every launch. NO until it was opened once.
- (BOOL)inspectorSlidersUnfolded:(NSString* _Nonnull)group;
- (void)setInspectorSliders:(NSString* _Nonnull)group unfolded:(BOOL)unfolded;

// CF1 -- a display UUID, retained across unplugging; nil disables output.
- (nullable NSString*)videoOutputScreen;
- (void)setVideoOutputScreen:(nullable NSString*)screen;

// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. The page the window shows,
// and for each page the panels left open on it and the widths the hand gave
// them. A page never visited answers its defaults (WorkspacePages.h); a
// width never dragged answers zero.
- (ui::workspace::Page)workspacePage;
- (void)setWorkspacePage:(ui::workspace::Page)page;
- (ui::workspace::PanelSet)panelsForPage:(ui::workspace::Page)page;
- (void)setPanels:(const ui::workspace::PanelSet&)panels
          forPage:(ui::workspace::Page)page;
- (ui::workspace::PanelWidths)panelWidthsForPage:(ui::workspace::Page)page;
- (void)setPanelWidths:(const ui::workspace::PanelWidths&)widths
               forPage:(ui::workspace::Page)page;

// ON3 -- docs/roadmap/ONGLETS-2026-10.md. Where the Agent's window was left,
// in screen coordinates (AgentWindow.h), none until it was first shown; and
// whether it was left shown, NO the first time.
- (std::optional<ui::agent_window::Rect>)agentWindowFrame;
- (void)setAgentWindowFrame:(const ui::agent_window::Rect&)frame;
- (BOOL)agentWindowShown;
- (void)setAgentWindowShown:(BOOL)shown;

// CH6 -- docs/roadmap/CHARTE-2026-11.md. Where the Charte panel was left,
// in screen coordinates (CharterWindow.h, whose Rect is the Agent window's),
// none until it was first shown. Not whether it was shown: a charter is set
// now and then, and the panel opens on demand only (DEC63).
- (std::optional<ui::agent_window::Rect>)charterWindowFrame;
- (void)setCharterWindowFrame:(const ui::agent_window::Rect&)frame;

@end
