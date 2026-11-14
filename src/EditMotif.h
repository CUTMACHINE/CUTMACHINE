#pragma once

// MOTIF1 -- docs/roadmap/MOTIF-2026-10.md. Why an edit was made, kept in the
// journal beside what it did.
//
// The document keeps what was done and never why: a cut made because a
// pause was too long and one made because the brief asked for twenty
// seconds look the same in the timeline and in its history. So a rule is
// corrected cut by cut instead of as a rule, a choice between variants is
// recorded without its reason, and a montage cannot be replayed as a
// sequence of intentions on other rushes.
//
// A motif is trace, not state (PHILOSOPHY.md, principles 1 and 2): it rides
// on a journal entry (EditLog.h), never on the document, so two projects
// whose timelines are byte-identical stay byte-identical whatever their
// motifs. It does not change what an operation does, and undo and redo move
// it with its entry.
//
// The reason is a closed vocabulary, drawn from the rules the engine runs
// (docs/roadmap/REGLES-2026-10.md, RM0) and from what decides a cut in the
// skills: a reader filters on it, which free text would not allow. What the
// vocabulary does not say goes in a short note.
//
// The journal spelling of these structs is described once, by FieldTables
// in OperationFormat.h.

#include "Json.h"
#include "Operations.h"
#include "Ulid.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class EditLog;
class Project;
class ProjectEditLog;

enum class MotifReason {
    // No reason was given. The default, never written.
    Unstated,
    // A filler, a false start, a word repeated (clean_disfluencies).
    Hesitation,
    // Silence between words taken out (tighten_pauses).
    Pause,
    // Where a cut enters and leaves the voice (trim_boundary_air,
    // close_junction_air).
    BoundaryAir,
    // The same idea said twice: the better take is kept.
    Duplicate,
    // What the film says: the passages kept, their order
    // (create_interview_short).
    Selection,
    // A cutaway laid over the voice.
    Cutaway,
    // Tempo: cuts on the music, a passage made shorter or faster.
    Rhythm,
    // The brief asked for it.
    Brief,
    // A marker someone placed asked for it (marker_id names it).
    Marker,
    // A format or a version: reframing, conforming, a declension.
    Format,
    // Captions laid by a rule (generate_captions).
    Captions,
    // A technical fix: sync, a flash frame, a broken join, a level.
    Technical,
    // None of the above: the note says what.
    Other,
};

enum class MotifAuthor {
    // An agent decided it.
    Agent,
    // A person decided it, the agent carrying it out.
    Person,
    // A written procedure decided it (get_procedure, a run step).
    Procedure,
};

struct EditMotif {
    MotifReason reason = MotifReason::Unstated;
    MotifAuthor author = MotifAuthor::Agent;
    // The tool whose rule decided the cut ("tighten_pauses"), set by that
    // tool's code and never by its caller; empty when no rule did.
    std::string rule;
    // The rule's parameters as the tool read them (thresholds included,
    // defaults filled in): a JSON object, or null when there are none.
    mcp_json::Value params;
    // What the reason does not say, in a line.
    std::string note;
    // The marker the edit answers, when the reason is a marker.
    Ulid marker_id;
};

// The members of one journal entry a motif covers: the whole entry for an
// operation, and for a batch the members [first, first + count) -- so that
// an intervention folded into one undo step (EditLog::GroupSince) keeps
// each call's motif on the members that call applied.
struct EntryMotif {
    uint64_t first = 0;
    uint64_t count = 1;
    EditMotif motif;
};

// A note is a line, not a document.
inline constexpr size_t kMaxMotifNoteCodePoints = 280;
// A rule's parameters are a handful of numbers.
inline constexpr size_t kMaxMotifParamsBytes = 4096;

// The journal's spelling of a reason or an author ("boundary_air").
const char* MotifReasonName(MotifReason reason);
const char* MotifAuthorName(MotifAuthor author);
bool ParseMotifReason(const std::string& name, MotifReason& reason);
bool ParseMotifAuthor(const std::string& name, MotifAuthor& author);
// Every reason a caller may give, in vocabulary order ("unstated" aside),
// for a schema and a refusal to list.
std::vector<std::string> MotifReasonNames();

// The journal's spelling of an entry's motifs, a JSON array, from the
// tables of OperationFormat.h (OperationFormat.cc).
std::string SerializeEntryMotifs(const std::vector<EntryMotif>& motifs);
bool DeserializeEntryMotifs(const std::string& json,
                            std::vector<EntryMotif>& motifs,
                            std::string& message);

// Whether a motif may enter a journal: params an object or null and under
// kMaxMotifParamsBytes, a note under kMaxMotifNoteCodePoints, a reason
// `other` with a note, and something said at all.
bool ValidateEditMotif(const EditMotif& motif, std::string& message);

// The `motif` argument of an edit tool (McpTools.cc): {reason, author,
// note, marker_id, params}, every key optional but one of reason or note.
// `rule` is refused: only a tool's code names its rule. `path` names the
// argument in a refusal ("tighten_pauses.motif").
bool ParseMotifArgument(const mcp_json::Value& value, const std::string& path,
                        EditMotif& motif, std::string& message);

// The motif as tool answers and list_motifs spell it: the journal's own
// keys, the defaults left out but `author`.
mcp_json::Value EditMotifJson(const EditMotif& motif);

// How many members an entry holds: a batch's, else 1.
size_t EntryMemberCount(const Operation& operation);

// What the journal records for one entry about to be applied: the spans
// already decided (`members`, from a plan's steps) and, over the members
// none of them covers, the call's motif. Spans sorted by `first`.
std::vector<EntryMotif> EntryMotifsFor(const Operation& operation,
                                       const std::vector<EntryMotif>& members,
                                       const std::optional<EditMotif>& motif);

// Whether `motifs` may ride on an entry of `members` members: each span
// non-empty, inside it, sorted and disjoint, each motif valid.
bool ValidateEntryMotifs(const std::vector<EntryMotif>& motifs, size_t members,
                         std::string& message);

// MOTIF4 -- what list_motifs answers about one timeline's journal: its
// entries in effect, grouped by reason, rule and parameters, each group
// with the entries and members it covers and what they touched. Undone
// entries are left out: they are not in the montage.
std::string ListMotifsJson(const Ulid& timelineId, const EditLog& log,
                           const std::optional<MotifReason>& reason,
                           const std::string& rule);

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. What the project's journal
// records for one entry about to be applied: the call's motif over its one
// member, or nothing.
std::vector<EntryMotif> ProjectEntryMotifsFor(
    const std::optional<EditMotif>& motif);

// MOTIF6 -- list_motifs with `journal: "project"`: the project journal's
// entries in effect, grouped as ListMotifsJson groups a timeline's, each
// member touched told by the timelines it names -- created, cloned,
// declined, removed, chosen and chosen among -- with their names in
// `project` as it is now (`removed: true` for one it no longer holds).
std::string ListProjectMotifsJson(const Project& project,
                                  const ProjectEditLog& log,
                                  const std::optional<MotifReason>& reason,
                                  const std::string& rule);
