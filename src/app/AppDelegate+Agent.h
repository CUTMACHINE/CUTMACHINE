#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The agent from
// the window: its floating window and toggle, the endpoint an outside agent
// reaches the open project through (AX-2026-09 AX1), and the journal of
// what it did, with its marks (PRD25, AgentJournal.h). Moved out of main.mm
// unchanged.

@interface AppDelegate (Agent)
- (void)buildAgentWindow;
- (void)toggleAgentWindow:(id)sender;
- (void)showAgentWindow:(BOOL)shown focus:(BOOL)focus;
- (void)agentDidWrite;
- (void)refreshAgentToggle;
- (void)agentWindowDidChangeFrame;
- (BOOL)agentWindowTakesKey:(NSEvent*)event;
// CH6 -- docs/roadmap/CHARTE-2026-11.md. The keys a field of the Charte
// panel keeps, and a montage command refused while the panel holds the
// keyboard.
- (BOOL)charterWindowTakesKey:(NSEvent*)event;
- (BOOL)charterRefusesMontageCommand:(id)sender;
- (void)startAgentEndpoint;
- (void)showAgentEndpointEvent:(const McpEndpoint::Event&)event;
- (std::string)agentJournalTime;
- (void)dismissAgentMarks;
@end
