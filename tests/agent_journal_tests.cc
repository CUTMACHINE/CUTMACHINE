// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The journal of what an
// agent relayed to the window did (AgentJournal.h): which calls enter it,
// the French line each one becomes, how an intervention folds, the bound,
// and the clips the timeline marks.
//
// The events are built as the endpoint builds them (McpEndpoint.h): a tool,
// its class in the catalog, and what its answer said. The endpoint's own
// reading of an answer is pinned by tests/mcp_endpoint_tests.cc, on a real
// socket; its delta reading is pinned here, since it is portable.
//
// Portable: no AppKit, no socket, no window.

#include "AgentJournal.h"
#include "Json.h"
#include "McpEndpoint.h"
#include "McpTools.h"

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) {
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

using Event = McpEndpoint::Event;
using C = McpToolClass;

const std::string kMain = "01KPRD25000000000000000001";
const std::string kOther = "01KPRD25000000000000000002";
const std::string kShotA = "01KPRD25000000000000000011";
const std::string kShotB = "01KPRD25000000000000000012";
const std::string kShotC = "01KPRD25000000000000000013";
const std::string kShotD = "01KPRD25000000000000000014";

AgentJournalContext Context(const std::string& time = "10:00:00") {
    AgentJournalContext context;
    context.time = time;
    context.timeline_name = [](const std::string& id) -> std::string {
        if (id == kMain) {
            return "Montage";
        }
        if (id == kOther) {
            return "Version courte";
        }
        return {};
    };
    return context;
}

// A call answered, as the endpoint reports it, on connection 1, while the
// window shows the main timeline. Each one moves the revision, as an edit
// does.
Event Call(const std::string& tool, C toolClass) {
    static int revision = 0;
    Event event;
    event.kind = Event::Kind::ToolCall;
    event.tool = tool;
    event.tool_class = toolClass;
    event.connection = 1;
    event.shown_timeline_id = kMain;
    event.previous_revision = "r" + std::to_string(revision);
    event.revision = "r" + std::to_string(++revision);
    return event;
}

Event Refused(const std::string& tool, C toolClass, const std::string& error) {
    Event event = Call(tool, toolClass);
    event.ok = false;
    event.error = error;
    event.revision.clear();
    return event;
}

Event Edit(const std::string& tool, const std::vector<std::string>& changed,
           const std::vector<std::string>& created = {},
           const std::vector<std::string>& removed = {}, int64_t shifted = 0) {
    Event event = Call(tool, C::JournaledEdit);
    event.has_delta = true;
    event.delta.changed = static_cast<int64_t>(changed.size());
    event.delta.created = static_cast<int64_t>(created.size());
    event.delta.removed = static_cast<int64_t>(removed.size());
    event.delta.shifted = shifted;
    event.delta.changed_clip_ids = changed;
    event.delta.created_clip_ids = created;
    event.delta.removed_clip_ids = removed;
    return event;
}

Event Begin(const std::string& token, const std::string& timeline = kMain) {
    Event event = Call("begin_intervention", C::Read);
    event.intervention = token;
    event.timeline_id = timeline;
    return event;
}

Event End(const std::string& token, const std::string& label) {
    Event event = Call("end_intervention", C::JournaledEdit);
    event.intervention = token;
    event.label = label;
    // The fold rewrites the history, not the montage.
    event.revision = event.previous_revision;
    return event;
}

std::vector<std::string> Lines(const AgentJournal& journal) {
    std::vector<std::string> lines;
    for (const AgentJournalEntry& entry : journal.Entries()) {
        lines.push_back(entry.text);
    }
    return lines;
}

std::string Joined(const std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& line : lines) {
        joined += "\n  " + line;
    }
    return joined;
}

std::set<std::string> Marks(const AgentJournal& journal) {
    return journal.MarkedClips();
}

}  // namespace

int main() {
    // ---- What an answer counts -----------------------------------------

    Test("a delta counts the clips it creates, changes, removes, shifts", [] {
        mcp_json::Value delta;
        std::string error;
        const bool parsed = mcp_json::Value::Parse(
            R"({"duration":{"value":100,"rate":25},)"
            R"("clips":[{"clip_id":"A","track_id":"T"},)"
            R"({"clip_id":"B","track_id":"T","created":true},)"
            R"({"clip_id":"C","track_id":"T","created":false}],)"
            R"("shifted":[{"track_id":"T","count":4},)"
            R"({"track_id":"U","count":3,"until":{"value":1,"rate":1}}],)"
            R"("removed_clip_ids":["D","E"]})",
            delta, error);
        Check(parsed, "the delta parses: " + error);
        const McpDeltaSummary summary = SummarizeMcpDelta(delta);
        Check(summary.changed == 2 && summary.created == 1 &&
                  summary.removed == 2 && summary.shifted == 7,
              "two changed, one created, two removed, seven shifted: " +
                  std::to_string(summary.changed) + " " +
                  std::to_string(summary.created) + " " +
                  std::to_string(summary.removed) + " " +
                  std::to_string(summary.shifted));
        Check(
            summary.changed_clip_ids == std::vector<std::string>{"A", "C"} &&
                summary.created_clip_ids == std::vector<std::string>{"B"} &&
                summary.removed_clip_ids == std::vector<std::string>{"D", "E"},
            "and names them, but for the shifted ones");
        const McpDeltaSummary nothing =
            SummarizeMcpDelta(mcp_json::Value::MakeString("delta"));
        Check(nothing.changed == 0 && nothing.created == 0 &&
                  nothing.removed == 0 && nothing.shifted == 0,
              "what is not a delta counts nothing");
    });

    Test("a class acts when a call of it can change something", [] {
        for (C acting : {C::Analysis, C::JournaledEdit, C::ProjectEdit,
                         C::FileWrite, C::Unjournaled, C::External}) {
            Check(McpToolClassActs(acting),
                  std::string(McpToolClassName(acting)) + " acts");
        }
        for (C quiet :
             {C::Unclassified, C::Read, C::Session, C::ExternalRead}) {
            Check(!McpToolClassActs(quiet),
                  std::string(McpToolClassName(quiet)) + " does not act");
        }
    });

    // ---- The line -------------------------------------------------------

    Test("the count reads in French, singular and plural right", [] {
        Check(AgentClipCountText(3, 1, 0, 0) == "3 plans modifiés, 1 créé",
              AgentClipCountText(3, 1, 0, 0));
        Check(AgentClipCountText(1, 0, 0, 0) == "1 plan modifié",
              AgentClipCountText(1, 0, 0, 0));
        Check(AgentClipCountText(0, 2, 0, 0) == "2 plans créés",
              AgentClipCountText(0, 2, 0, 0));
        Check(AgentClipCountText(0, 0, 1, 12) == "1 plan retiré, 12 décalés",
              AgentClipCountText(0, 0, 1, 12));
        Check(AgentClipCountText(2, 2, 2, 1) ==
                  "2 plans modifiés, 2 créés, 2 retirés, 1 décalé",
              AgentClipCountText(2, 2, 2, 1));
        Check(AgentClipCountText(0, 0, 0, 0).empty(), "nothing is nothing");
    });

    Test("an edit is one line: the time, what was done, what it counted", [] {
        AgentJournal journal;
        Check(journal.Record(Edit("move_clip", {kShotA}), Context()),
              "an edit enters");
        Check(Lines(journal) == std::vector<std::string>{"10:00:00 Déplacer "
                                                         "un plan — 1 plan "
                                                         "modifié"},
              "the line:" + Joined(Lines(journal)));
        journal.Record(Edit("split_clip", {kShotA}, {kShotB}),
                       Context("10:00:07"));
        Check(Lines(journal).back() ==
                  "10:00:07 Couper un plan — 1 plan modifié, 1 créé",
              Lines(journal).back());
        journal.Record(Edit("set_clip_effects", {kShotC}), Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Étalonner un plan — 1 plan modifié",
              "a title that starts with an accent takes its capital: " +
                  Lines(journal).back());
        journal.Record(Edit("add_marker", {}), Context());
        Check(Lines(journal).back() == "10:00:00 Poser un marqueur",
              "an edit of no clip counts nothing: " + Lines(journal).back());
        journal.Record(Edit("tighten_pauses", {kShotA}, {}, {kShotB}, 40),
                       Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Resserrer les pauses — 1 plan modifié, 1 retiré, "
                  "40 décalés",
              Lines(journal).back());
    });

    Test("reads, the session's bookkeeping and unknown tools never enter", [] {
        AgentJournal journal;
        journal.Record(Edit("move_clip", {kShotA}), Context());
        const auto before = Lines(journal);
        Check(
            !journal.Record(Call("describe", C::Read), Context()) &&
                !journal.Record(Call("get_editor_view", C::Read), Context()) &&
                !journal.Record(Call("read_frame", C::Read), Context()) &&
                !journal.Record(Call("open_project", C::Session), Context()) &&
                !journal.Record(Call("cancel_task", C::Session), Context()) &&
                !journal.Record(Call("after_list_comps", C::ExternalRead),
                                Context()) &&
                !journal.Record(Call("no_such_tool", C::Unclassified),
                                Context()) &&
                !journal.Record(Refused("describe", C::Read, "UnknownClip"),
                                Context()),
            "none of them changes the journal");
        Check(Lines(journal) == before, "the lines are the ones before");
        Check(Marks(journal) == std::set<std::string>{kShotA}, "nor the marks");
    });

    Test("a refusal reads as a refusal, with the error's name", [] {
        AgentJournal journal;
        journal.Record(Edit("move_clip", {kShotA}), Context());
        Check(journal.Record(
                  Refused("remove_clip", C::JournaledEdit, "UnknownClip"),
                  Context("10:01:00")),
              "a refused edit enters");
        Check(Lines(journal).back() ==
                  "10:01:00 Refusé : supprimer un plan (UnknownClip)",
              Lines(journal).back());
        Check(journal.Entries().back().refused &&
                  !journal.TranscriptEntries().back().tool_ok,
              "and is a refusal to the panel");
        Check(Marks(journal) == std::set<std::string>{kShotA},
              "it touched nothing: the marks stay");
        Event plan =
            Refused("apply_edit_plan", C::JournaledEdit, "StaleRevision");
        plan.label = "Couper les euh";
        journal.Record(plan, Context());
        Check(
            Lines(journal).back() ==
                "10:00:00 Refusé : « Couper les euh » (StaleRevision)",
            "a labelled call is named by its label: " + Lines(journal).back());
    });

    Test("a replay, or an edit that moved no revision, enters nothing", [] {
        AgentJournal journal;
        Event replay = Edit("move_clip", {kShotA});
        replay.replayed = true;
        Check(!journal.Record(replay, Context()) && journal.Entries().empty(),
              "a replay applied nothing again");
        // propose_grade without apply: an edit's class, a read's effect.
        Event proposal = Call("propose_grade", C::JournaledEdit);
        proposal.revision = proposal.previous_revision;
        Check(!journal.Record(proposal, Context()) && journal.Entries().empty(),
              "an edit that left the project as it was");
        Event first = Call("propose_grade", C::JournaledEdit);
        first.previous_revision.clear();
        Check(journal.Record(first, Context()) &&
                  Lines(journal).back() ==
                      "10:00:00 Appliquer un étalonnage proposé",
              "with no revision to compare, the call is kept: " +
                  Joined(Lines(journal)));
        Event exported = Call("export_timeline", C::FileWrite);
        exported.revision = exported.previous_revision;
        Check(journal.Record(exported, Context()),
              "an export changes no revision and still enters");
    });

    Test("an analysis or an export started in the background says so", [] {
        AgentJournal journal;
        Event transcription = Call("transcribe_media", C::Analysis);
        transcription.background = true;
        journal.Record(transcription, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Transcrire des rushes — en tâche de fond",
              Lines(journal).back());
        journal.Record(Call("export_timeline", C::FileWrite), Context());
        Check(Lines(journal).back() == "10:00:00 Exporter la timeline",
              Lines(journal).back());
        Check(Marks(journal).empty(), "neither marks a clip");
    });

    Test("the timeline is named when the window showed another one", [] {
        AgentJournal journal;
        Event elsewhere = Edit("move_clip", {kShotA});
        elsewhere.timeline_id = kOther;
        journal.Record(elsewhere, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Déplacer un plan — timeline « Version courte » — "
                  "1 plan modifié",
              Lines(journal).back());
        Event shown = Edit("move_clip", {kShotA});
        shown.timeline_id = kMain;
        journal.Record(shown, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Déplacer un plan — 1 plan "
                  "modifié",
              "the one shown is not named: " + Lines(journal).back());
        Event unnamed = Edit("move_clip", {kShotA});
        journal.Record(unnamed, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Déplacer un plan — 1 plan "
                  "modifié",
              "nor the active one, which a call that names none edits");
        Event headless = Edit("move_clip", {kShotA});
        headless.timeline_id = kMain;
        headless.shown_timeline_id.clear();
        journal.Record(headless, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Déplacer un plan — timeline « Montage » — 1 plan "
                  "modifié",
              "a window that cannot say what it shows names it: " +
                  Lines(journal).back());
        Event gone = Call("remove_timeline", C::ProjectEdit);
        gone.timeline_id = "01KPRD25000000000000000099";
        journal.Record(gone, Context());
        Check(Lines(journal).back() == "10:00:00 Supprimer une timeline",
              "a timeline the project no longer names is left out: " +
                  Lines(journal).back());
        Event refused = Refused("move_clip", C::JournaledEdit, "Overlap");
        refused.timeline_id = kOther;
        journal.Record(refused, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 Refusé : déplacer un plan — timeline « Version "
                  "courte » (Overlap)",
              Lines(journal).back());
    });

    // ---- Interventions --------------------------------------------------

    Test("an intervention folds into one entry with its label", [] {
        AgentJournal journal;
        journal.Record(Edit("set_clip_opacity", {kShotD}), Context("09:59:00"));
        Check(!journal.Record(Begin("tok-1"), Context()),
              "beginning writes nothing");
        journal.Record(Edit("set_clip_opacity", {kShotA}), Context());
        journal.Record(Edit("split_clip", {kShotB}, {kShotC}), Context());
        journal.Record(Edit("set_clip_opacity", {kShotA}), Context());
        Check(journal.Entries().size() == 4,
              "each step shows as it comes:" + Joined(Lines(journal)));
        Check(Marks(journal) == std::set<std::string>{kShotA},
              "the marks follow the last step");
        Check(journal.Record(End("tok-1", "Trois pas de l'agent"),
                             Context("10:00:09")),
              "ending folds");
        Check(Lines(journal) ==
                  std::vector<std::string>{
                      "09:59:00 Régler l'opacité — 1 plan modifié",
                      "10:00:09 « Trois pas de l'agent » — 2 plans modifiés, "
                      "1 créé"},
              "the steps make way for one entry:" + Joined(Lines(journal)));
        Check(Marks(journal) == std::set<std::string>{kShotA, kShotB, kShotC},
              "and the marks are what the intervention touched");
        Check(journal.Entries().back().touched_clip_ids.size() == 3,
              "the entry names them");
    });

    Test("a clip created and removed within an intervention was never there",
         [] {
             AgentJournal journal;
             journal.Record(Begin("tok-2"), Context());
             journal.Record(Edit("split_clip", {kShotA}, {kShotB}), Context());
             journal.Record(Edit("remove_clip", {}, {}, {kShotB}, 3),
                            Context());
             journal.Record(Edit("remove_clip", {}, {}, {kShotC}), Context());
             journal.Record(End("tok-2", "Nettoyer"), Context());
             Check(Lines(journal) ==
                       std::vector<std::string>{
                           "10:00:00 « Nettoyer » — 1 plan modifié, 1 retiré"},
                   "created then removed is neither; a ripple is not "
                   "counted:" +
                       Joined(Lines(journal)));
             Check(Marks(journal) == std::set<std::string>{kShotA},
                   "only what stays is marked");
         });

    Test("a refusal within an intervention stays a line of its own", [] {
        AgentJournal journal;
        journal.Record(Begin("tok-3"), Context());
        journal.Record(Edit("move_clip", {kShotA}), Context());
        journal.Record(Refused("move_clip", C::JournaledEdit, "Overlap"),
                       Context());
        journal.Record(End("tok-3", "Déplacer"), Context());
        Check(Lines(journal) ==
                  std::vector<std::string>{
                      "10:00:00 Refusé : déplacer un plan (Overlap)",
                      "10:00:00 « Déplacer » — 1 plan modifié"},
              Joined(Lines(journal)));
    });

    Test("a refused end leaves the steps apart, and can be ended again", [] {
        AgentJournal journal;
        journal.Record(Begin("tok-4"), Context());
        journal.Record(Edit("move_clip", {kShotA}), Context());
        journal.Record(Edit("move_clip", {kShotB}), Context());
        Event refused = End("tok-4", "Deux pas");
        refused.ok = false;
        refused.error = "InvalidOperation";
        journal.Record(refused, Context());
        Check(Lines(journal) ==
                  std::vector<std::string>{
                      "10:00:00 Déplacer un plan — 1 plan modifié",
                      "10:00:00 Déplacer un plan — 1 plan modifié",
                      "10:00:00 Refusé : regrouper « Deux pas » "
                      "(InvalidOperation)"},
              Joined(Lines(journal)));
        journal.Record(End("tok-4", "Deux pas"), Context());
        Check(Lines(journal) ==
                  std::vector<std::string>{
                      "10:00:00 Refusé : regrouper « Deux pas » "
                      "(InvalidOperation)",
                      "10:00:00 « Deux pas » — 2 plans modifiés"},
              "ended at last:" + Joined(Lines(journal)));
    });

    Test("only the intervention's own agent and timeline fold into it", [] {
        AgentJournal journal;
        journal.Record(Begin("tok-5"), Context());
        Event other = Edit("move_clip", {kShotB});
        other.connection = 2;
        journal.Record(other, Context());
        Event elsewhere = Edit("move_clip", {kShotC});
        elsewhere.timeline_id = kOther;
        journal.Record(elsewhere, Context());
        journal.Record(Call("export_timeline", C::FileWrite), Context());
        journal.Record(Edit("move_clip", {kShotA}), Context());
        journal.Record(End("tok-5", "Le mien"), Context());
        Check(Lines(journal) ==
                  std::vector<std::string>{
                      "10:00:00 Déplacer un plan — 1 plan modifié",
                      "10:00:00 Déplacer un plan — timeline « Version courte "
                      "» — 1 plan modifié",
                      "10:00:00 Exporter la timeline",
                      "10:00:00 « Le mien » — 1 plan modifié"},
              Joined(Lines(journal)));
    });

    Test("an agent that leaves mid-intervention leaves its steps apart", [] {
        AgentJournal journal;
        journal.Record(Begin("tok-6"), Context());
        journal.Record(Edit("move_clip", {kShotA}), Context());
        Event gone;
        gone.kind = Event::Kind::Disconnected;
        gone.connection = 1;
        Check(!journal.Record(gone, Context()), "a departure writes nothing");
        journal.Record(Edit("move_clip", {kShotB}), Context());
        Check(journal.Entries().size() == 2, Joined(Lines(journal)));
        journal.Record(End("tok-6", "Trop tard"), Context());
        Check(Lines(journal).back() == "10:00:00 « Trop tard »" &&
                  journal.Entries().size() == 3,
              "an end the journal saw no beginning of folds nothing:" +
                  Joined(Lines(journal)));
    });

    Test("a plan applied reads the delta its preparation answered", [] {
        AgentJournal journal;
        Event prepared = Call("prepare_edit_plan", C::Session);
        prepared.plan_id = "plan-1";
        prepared.label = "Couper les euh";
        prepared.has_delta = true;
        prepared.delta.changed = 2;
        prepared.delta.removed = 1;
        prepared.delta.shifted = 9;
        prepared.delta.changed_clip_ids = {kShotA, kShotB};
        prepared.delta.removed_clip_ids = {kShotC};
        Check(!journal.Record(prepared, Context()), "preparing writes nothing");
        Event applied = Call("apply_edit_plan", C::JournaledEdit);
        applied.plan_id = "plan-1";
        applied.label = "Couper les euh";
        applied.delta_as_prepared = true;
        journal.Record(applied, Context());
        Check(Lines(journal).back() ==
                  "10:00:00 « Couper les euh » — 2 plans modifiés, 1 retiré, "
                  "9 décalés",
              Lines(journal).back());
        Check(Marks(journal) == std::set<std::string>{kShotA, kShotB},
              "and marks what it changed");
        Event unknown = applied;
        unknown.plan_id = "plan-9";
        journal.Record(unknown, Context());
        Check(Lines(journal).back() == "10:00:00 « Couper les euh »",
              "a plan the journal did not see prepared counts nothing: " +
                  Lines(journal).back());
    });

    // ---- The bound, the marks, the panel ---------------------------------

    Test("the journal keeps the last 200 entries, the oldest going first", [] {
        AgentJournal journal;
        for (int index = 0; index < 205; ++index) {
            journal.Record(Edit("move_clip", {kShotA}),
                           Context(std::to_string(index)));
        }
        Check(journal.Entries().size() == AgentJournal::kCapacity &&
                  AgentJournal::kCapacity == 200,
              std::to_string(journal.Entries().size()) + " entries");
        Check(journal.Entries().front().text.rfind("5 ", 0) == 0 &&
                  journal.Entries().back().text.rfind("204 ", 0) == 0,
              "from the sixth to the last: " + journal.Entries().front().text);
        bool increasing = true;
        for (size_t index = 1; index < journal.Entries().size(); ++index) {
            increasing = increasing && journal.Entries()[index].id >
                                           journal.Entries()[index - 1].id;
        }
        Check(increasing, "each entry has an ID of its own, increasing");
    });

    Test("the marks go when the person acts in the timeline", [] {
        AgentJournal journal;
        Check(!journal.DismissMarks(), "no mark, nothing to dismiss");
        journal.Record(Edit("split_clip", {kShotA}, {kShotB}), Context());
        Check(Marks(journal) == std::set<std::string>{kShotA, kShotB},
              "a cut marks both halves");
        Check(journal.DismissMarks() && Marks(journal).empty(),
              "the person's click takes them away");
        Check(journal.Entries().size() == 1, "and leaves the journal whole");
        journal.Record(Edit("move_clip", {kShotC}), Context());
        Check(Marks(journal) == std::set<std::string>{kShotC},
              "the next call marks again");
    });

    Test("the panel reads the journal's lines, as lines of their own kind", [] {
        AgentJournal journal;
        journal.Record(Edit("move_clip", {kShotA}), Context());
        journal.Record(Refused("remove_clip", C::JournaledEdit, "UnknownClip"),
                       Context());
        const std::vector<chat::ChatTranscriptEntry> rows =
            journal.TranscriptEntries();
        Check(rows.size() == 2, "one row an entry");
        for (size_t index = 0; index < rows.size() && index < 2; ++index) {
            Check(rows[index].kind == chat::ChatEntryKind::AgentJournal &&
                      rows[index].text == journal.Entries()[index].text &&
                      rows[index].journal_id == journal.Entries()[index].id,
                  "row " + std::to_string(index) + " is its entry");
        }
        Check(rows.size() == 2 && rows[0].tool_ok && !rows[1].tool_ok,
              "a refusal is not ok");
    });

    Test("every tool that can change something has its wording", [] {
        const McpToolRegistry registry;
        size_t acting = 0;
        for (const McpTool& tool : registry.Tools()) {
            if (!McpToolClassActs(tool.tool_class)) {
                continue;
            }
            ++acting;
            const std::string title = AgentToolTitle(tool.name);
            Check(title != tool.name,
                  tool.name + " has no French wording in AgentJournal.cc");
            // Lower case, as a refusal reads it; the capital is the line's.
            const unsigned char first =
                title.empty() ? 0 : static_cast<unsigned char>(title[0]);
            Check((first >= 'a' && first <= 'z') ||
                      (first == 0xC3 && title.size() > 1 &&
                       static_cast<unsigned char>(title[1]) == 0xA9),
                  tool.name +
                      "'s wording starts with a letter the journal "
                      "can capitalise: " +
                      title);
        }
        Check(acting > 100, std::to_string(acting) + " tools that act");
        Check(AgentToolTitle("no_such_tool") == "no_such_tool",
              "a tool the journal does not know is named as it is");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All agent journal tests passed\n";
    return 0;
}
