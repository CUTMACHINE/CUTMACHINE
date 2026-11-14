#pragma once

// What the Agent panel shows, whichever agent produced it (ROADMAP.md F2.4,
// docs/roadmap/ACP-2026-09.md). The built-in tool loop (ChatSession.h) and
// the client of an external coding agent (AcpClient.h) both report their turn
// as this list, so the panel draws and logs one kind of line.
//
// Plain C++, no dependency: the ACP client links it on a host without the
// rest of the chat stack.

#include <cstdint>
#include <string>

namespace chat {

enum class ChatEntryKind {
    UserMessage,    // the instruction that started this turn
    AssistantText,  // a text block the model produced
    ToolCall,    // a tool the model asked to run (before the result is known)
    ToolResult,  // that tool call's outcome
    Error,       // the turn stopped early: transport/protocol/safety-limit
    // ACP-2026-09 -- the agent's own plan of the turn, as the text of every
    // step with its state. An agent sends the whole plan again at each change;
    // the panel keeps one plan line per turn and rewrites it.
    Plan,
    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). A line of the journal
    // of what an agent relayed to the window did (AgentJournal.h): not a
    // turn of the panel's own agent, and not written by this list's
    // producers. The panel keeps these rows apart: they come and go with the
    // journal, a folded intervention replacing its steps.
    AgentJournal,
};

// One line of the transcript the chat panel renders. Which fields are
// meaningful depends on `kind`, documented per field below.
struct ChatTranscriptEntry {
    ChatEntryKind kind = ChatEntryKind::UserMessage;
    std::string text;            // UserMessage/AssistantText/Error/Plan text;
                                 // ToolResult's success or refusal JSON;
                                 // AgentJournal's whole line
    std::string tool_name;       // ToolCall/ToolResult only
    std::string tool_args_json;  // ToolCall only, for display
    bool tool_ok = false;        // ToolResult, and AgentJournal (false: a
                                 // refusal)
    // AgentJournal only: the journal entry's ID (AgentJournalEntry::id).
    uint64_t journal_id = 0;
    // ToolResult only: the picture the tool returned to the model (read_frame,
    // contact_sheet...), shown in the panel too (visual QA, 19 September
    // 2026: the agent saw it, the user did not).
    std::string image_base64;
    std::string image_mime;
};

}  // namespace chat
