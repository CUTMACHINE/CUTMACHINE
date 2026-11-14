#pragma once

// AppKit-only. This file and ChatPanelView.mm compile against a real macOS
// SDK but have not been exercised at runtime -- see VISUAL_QA_CHECKLIST.md.
// The logic underneath them is covered by tests and does not depend on
// AppKit (ChatLlmClient.h/ChatSession.h/McpLiveBackend.h, all plain C++).
//
// The chat panel (ROADMAP.md F2.4): a message transcript plus a text input
// and send button, styled from UiTheme.h/UiComponents.h like every other
// F2.x panel. It was the right dock's second tab; since ON3
// (docs/roadmap/ONGLETS-2026-10.md) it is the content of the Agent's own
// floating window (AgentWindow.h), PanelLayout.h's floating slot.
//
// This view owns no document-mutation logic of its own. Every edit an
// instruction causes is dispatched by ChatSession through
// McpToolRegistry::Call -- the exact function McpServer.cc's `tools/call`
// JSON-RPC handler already uses -- against whatever McpBackend
// -configureWithBackend: was given. This view's only responsibilities are
// rendering the transcript, reading the input field, and getting each turn
// off the main thread (network I/O) while still running the resulting
// document mutation back on it (see ChatPanelView.mm's MainThreadBackend).
//
// ACP3 -- docs/roadmap/ACP-2026-09.md. The engine can also be an external
// coding agent (Claude Code, Codex...) that speaks the Agent Client Protocol
// (AcpClient.h): the panel starts it, hands it the engine's MCP server --
// this binary in `--mcp-serve <project> --stdio`, which relays to the
// window's endpoint (AX1) -- shows its turn in the same transcript, puts to
// the person any call that is not the engine's, and folds each turn into
// one Undo as it does for the built-in loop.

#import <AppKit/AppKit.h>

#include "AiEngineForm.h"
#include "ChatTranscript.h"
#include "McpLiveBackend.h"

#include <vector>

@class CMFormPanel;

// PRD22 -- ROADMAP.md. The form of the "Moteur IA" dialog, held by the
// window's form panel (PRD26, FormPanel.h). AiEngineForm.h decides which
// fields a protocol uses, what each suggests and what the protocol menu
// brings back; this view applies it at opening and on every move of the
// menu. Public for the window test,
// which builds it as -showConfigurationWindow does, moves the menu and reads
// the fields by their identifiers (ai_engine.provider, .acp_command, .model,
// .base_url, .api_key, .remove_key; a text field's label is its identifier
// followed by ".label").
@interface CMAiEngineFormView : NSView

// `drafts`: what each protocol's fields hold at opening
// (ai_engine::DraftsFromStored). `context`: what an empty field stands for
// and whether a key is already in effect, which only changes placeholders.
- (instancetype)initWithProtocol:(ai_engine::EngineProtocol)protocol
                          drafts:(const ai_engine::FormDrafts&)drafts
                         context:(const ai_engine::FormContext&)context;

// The protocol the menu shows, and what its fields hold, untrimmed.
- (ai_engine::EngineProtocol)selectedProtocol;
- (ai_engine::FormTexts)texts;
- (BOOL)removesStoredKey;

// A new form panel that holds this form: title, help text, Enregistrer and
// Annuler. A new one per showing, the form moving into it.
- (CMFormPanel*)makePanel;

@end

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. Whether an Anthropic
// key is saved in this panel's settings, in the Keychain: a fact the setup
// notice adds to the engine's report, which reads ANTHROPIC_API_KEY only
// (SetupNotice.h). The query asks for no data, so the key is never read.
// Callable off the main thread.
BOOL CMAgentAnthropicKeySaved(void);

@interface CMChatPanelView : NSView

- (instancetype)initWithFrame:(NSRect)frame;

// Wires the panel to a live document. `backend` is not owned and must
// outlive this view -- main.mm constructs an McpLiveBackend
// (McpLiveBackend.h) over `self.state->document`/`self.state->editLog` and
// passes it here once, right where the chat view is installed into the
// right dock. `self.state` is allocated once before the window is built and
// never reallocated afterward (only its fields are mutated by ordinary
// edits and project loads), so that reference stays valid for the window's
// whole lifetime. Call once, before the view is shown. The live backend and
// not any McpBackend, because the transcription tools read what they need of
// it on the main thread and run the rest on the chat worker (CHAT-PARITE).
- (void)configureWithBackend:(McpLiveBackend&)backend;

// Presents the local BYOK configuration. The model and endpoint are ordinary
// interface preferences; the API key is stored in the macOS Keychain and is
// never written to a project or NSUserDefaults. The command that starts an
// ACP agent is an interface preference too; the agent logs in with its own
// account, which CUTMACHINE never sees.
- (void)showConfigurationWindow;
// Called on the main thread when the user stops a running turn: cancels the
// media tasks that turn started (visual QA, 19 September 2026).
- (void)setStopHandler:(void (^)(void))handler;

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The journal of what an
// agent relayed to the window did, as AgentJournal::TranscriptEntries gives
// it, whole, after each change: rows of entries no longer there go (a folded
// intervention's steps, an entry past the bound), new ones are added at the
// end of the thread, among the panel's own turns. Fed whether the panel is
// shown or not, so it shows what happened when it is opened. Each row's
// identifier is "chat.agent_journal", for the window test. Not written to
// the panel's log of its own conversations: the journal is a state of the
// interface, gone with the window.
- (void)showAgentJournal:(const std::vector<chat::ChatTranscriptEntry>&)entries;

// ON3 -- the window this panel's forms are sheets on (« Moteur IA »): the
// editor's, as every form of the window (FormPanel.h), rather than the
// Agent's own, which may be hidden or too narrow. Its own window when unset.
@property(nonatomic, weak) NSWindow* formWindow;
// ON3 -- called on the main thread whenever a row is added to the thread,
// the panel's own turns and the journal of a relayed agent alike: the window
// marks the Agent's toggle while its window is hidden.
@property(nonatomic, copy) void (^activityHandler)(void);
// ON3 -- the keyboard in the request field, the Agent's window shown for a
// request.
- (void)focusInput;

@end
