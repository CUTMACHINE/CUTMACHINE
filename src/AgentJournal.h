#pragma once

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). What an agent did, as the
// editor reads it in the Agent panel.
//
// An agent outside the window -- a Claude Code session relayed to the app
// (McpEndpoint.h, AX1) -- edits the project on screen. The window used to
// say "Agent : <last tool>" in a status line that eighty other places of
// main.mm rewrite, and nothing of what changed nor where. CUTMACHINE's
// promise is to hand a series of montages over to agents; the window is
// where their work is read back.
//
// This file decides what the journal holds: which calls enter it, the French
// line each one becomes, how the calls of one intervention fold into one
// entry, and which clips the timeline marks as just touched. The window only
// feeds it the endpoint's events and draws what it answers (main.mm,
// ChatPanelView.mm, ClipBadges.h).
//
// An interface state: in memory, bounded, gone when the window closes.
// Nothing of it is written to the document or to the preferences. The
// durable history of a timeline is its journal of operations, which
// `describe` reads with only: ["history"] (PRD35).
//
// What enters is decided from what the catalog declares of each tool
// (McpToolClass, PRD15) and from what the call's answer says, never from a
// list of tool names: every call that can change something -- an edit, a
// project edit, an export, an analysis started, a write to another program
// -- and every refusal of one. A read never enters, nor the session's
// bookkeeping (a plan prepared, a task cancelled), nor a replayed call, nor
// an edit whose answer left the project at the revision the previous answer
// had (propose_grade without apply: a read in an edit's clothes). The
// wording of each tool is a table, since a sentence cannot be derived; a
// tool it lacks is named as the catalog names it.
//
// Portable: no AppKit (tests/agent_journal_tests.cc).

#include "ChatTranscript.h"
#include "McpEndpoint.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// What the caller knows and the journal does not read itself.
struct AgentJournalContext {
    // The local time of the line, "HH:MM:SS": read by the caller when the
    // event reaches it, never here, so that a test writes the line it
    // expects.
    std::string time;
    // A timeline's name, as the project names it; empty when the project
    // has none by that ID (a timeline just removed), and the line then names
    // no timeline.
    std::function<std::string(const std::string& timelineId)> timeline_name;
};

struct AgentJournalEntry {
    // Unique and increasing, never reused: the panel keys its rows on it, so
    // a folded intervention replaces its steps' rows and an entry past the
    // bound takes its row with it.
    uint64_t id = 0;
    // The whole line, the time first.
    std::string text;
    bool refused = false;
    // The clips this entry created or changed, by ID.
    std::vector<std::string> touched_clip_ids;
};

class AgentJournal {
public:
    // The entries kept; the oldest goes first.
    static constexpr size_t kCapacity = 200;
    // The plans prepared whose delta is kept for apply_edit_plan's short
    // answer: a session holds eight at most (McpTools.h).
    static constexpr size_t kRememberedPlans = 16;

    // Takes one event of the window's endpoint, in the order they came.
    // True when the entries or the marks changed.
    bool Record(const McpEndpoint::Event& event,
                const AgentJournalContext& context);

    // Oldest first.
    const std::deque<AgentJournalEntry>& Entries() const { return entries_; }

    // The entries as the Agent panel shows them: ChatEntryKind::AgentJournal,
    // `journal_id` the entry's ID, `tool_ok` false for a refusal.
    std::vector<chat::ChatTranscriptEntry> TranscriptEntries() const;

    // The clips the timeline marks: those created or changed by the last
    // entry that touched any. A refusal, a read, an export or an edit of no
    // clip leaves them as they are; the next call that touches clips
    // replaces them.
    const std::set<std::string>& MarkedClips() const { return marked_; }
    // The person acted in the timeline: the marks go. True when there were
    // some.
    bool DismissMarks();

private:
    // An intervention begun and not yet ended (begin_intervention), and what
    // its steps did, kept apart from the entries so that a step pushed out by
    // the bound still counts.
    struct Intervention {
        std::string token;
        uint64_t connection = 0;
        std::string timeline_id;
        // The timeline the window showed as its first step came in.
        std::string shown_timeline_id;
        bool stepped = false;
        std::set<std::string> created;
        std::set<std::string> changed;
        std::set<std::string> removed;
        std::vector<uint64_t> steps;
    };

    bool Close(const McpEndpoint::Event& event,
               const AgentJournalContext& context);
    Intervention* InterventionOf(const McpEndpoint::Event& event);
    // Numbers the entry, keeps it, and answers its ID.
    uint64_t Append(AgentJournalEntry entry);
    void RememberPlan(const std::string& planId, const McpDeltaSummary& delta);

    std::deque<AgentJournalEntry> entries_;
    std::set<std::string> marked_;
    std::vector<Intervention> open_;
    std::deque<std::pair<std::string, McpDeltaSummary>> plans_;
    uint64_t last_id_ = 0;
};

// The policy's pieces, exposed for the tests.

// What a call of `tool` does, in French and in lower case, as a refusal
// reads it ("déplacer un plan"); the tool's own name when the journal has no
// wording for it.
std::string AgentToolTitle(const std::string& tool);

// What an entry counts, its first figure naming the clips and each of the
// others agreeing with it: "3 plans modifiés, 1 créé", "1 plan retiré, 12
// décalés". Changed, created, removed, then shifted; a figure of zero is
// left out, and nothing at all is the empty string.
std::string AgentClipCountText(int64_t changed, int64_t created,
                               int64_t removed, int64_t shifted);
