#pragma once

#include "DocumentDelta.h"
#include "EditMotif.h"
#include "Operations.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct Entry {
    Operation op;
    Operation inverse;
    // MOTIF1 -- docs/roadmap/MOTIF-2026-10.md. Why the entry was made, over
    // the members each motif covers (EditMotif.h); empty when nobody said.
    // Trace, not state: undo and redo carry it with the entry, and nothing
    // in the document depends on it.
    std::vector<EntryMotif> motifs;
};

// MOTIF1 -- the journal's versions. A journal whose entries carry no motif
// is written as version 1, byte for byte what every build before MOTIF1
// wrote and reads; one that carries a motif is version 2. A build before
// MOTIF1 refuses version 2 (its reader expects version 1), as it refuses a
// document newer than it reads: the oldest version that can carry what the
// journal holds is the one written, so a project edited without motifs stays
// readable by an older build.
inline constexpr int kEditLogVersion = 1;
inline constexpr int kEditLogVersionWithMotifs = 2;

// PL1 -- ROADMAP.md. Where an intervention starts in one timeline's history:
// how many entries were applied, and which entry was on top, by the
// CanonicalHash of its text. Nothing is written when an intervention begins,
// so a mark survives the process that took it -- each `--call-tool` is a
// process of its own -- and an intervention nobody ends leaves the history
// as it was.
//
// The mark names a place in the history, not a state of the document: a
// rush ingested or a timeline renamed during the intervention changes the
// document without touching this history, and must not stop the fold.
struct InterventionMark {
    Ulid timeline_id;
    size_t history_depth = 0;
    std::string history_anchor;
};

class EditLog;

InterventionMark MarkIntervention(const Ulid& timelineId, const EditLog& log);
// The token an agent carries from begin_intervention to end_intervention:
// "<timeline_id>:<history_depth>:<history_anchor>".
std::string FormatInterventionMark(const InterventionMark& mark);
bool ParseInterventionMark(const std::string& token, InterventionMark& mark,
                           std::string& message);
// What begin_intervention and end_intervention answer, on every surface;
// `doc_hash` is the state the timeline is in, as every edit result names it.
std::string InterventionBeganJson(const InterventionMark& mark,
                                  const Document& document);
std::string InterventionEndedJson(size_t grouped, const EditLog& log,
                                  const Document& document);

class EditLog {
public:
    // MOTIF1 -- `motifs` ride on the entry (EntryMotifsFor builds them); a
    // span outside the operation's members, or an invalid motif, refuses the
    // edit before it is applied.
    bool Apply(Document& document, Operation operation, EditError& error,
               std::string& message, std::vector<EntryMotif> motifs = {});
    bool Undo(Document& document, EditError& error, std::string& message);
    bool Redo(Document& document, EditError& error, std::string& message);

    // PL1 -- ROADMAP.md. Folds every entry applied since `mark` into one
    // BatchOperation entry labelled `label`, so that one Undo reverts the
    // whole intervention and one Redo replays it. `document` is the state
    // the log has led to; it does not change, only the history does.
    //
    // Refused, with the history left as it was, when the history was undone
    // below the mark or rewritten under it (another entry where the mark's
    // anchor was), when the intervention holds more than
    // kMaxBatchOperations operations, or when the fold would not undo and
    // redo exactly (checked on a copy first). Batches among the entries are
    // flattened, never nested. PRD40 -- a single entry is folded as well,
    // into a batch of one, so that the label survives whatever the
    // intervention's size; zero entries leave the history as it is.
    // `grouped` is the number of entries the intervention spans.
    //
    // MOTIF1 -- each folded entry's motifs are kept on the members it
    // lends, and `motif`, when given, covers the members none of them
    // covers: a fold loses no reason.
    bool GroupSince(const Document& document, const InterventionMark& mark,
                    const std::string& label, size_t& grouped, EditError& error,
                    std::string& message,
                    const std::optional<EditMotif>& motif = std::nullopt);

    size_t AppliedCount() const;
    size_t UndoneCount() const;
    const std::vector<Entry>& AppliedEntries() const;
    const std::vector<Entry>& UndoneEntries() const;

    bool Save(const std::string& path, EditError& error,
              std::string& message) const;
    static bool Load(const std::string& path, EditLog& output, EditError& error,
                     std::string& message);
    std::string Serialize() const;
    static bool Deserialize(const std::string& json, EditLog& output,
                            EditError& error, std::string& message);

private:
    std::vector<Entry> applied_;
    std::vector<Entry> undone_;
};

// PRD35 -- ROADMAP.md. A timeline's history as describe's `history` reads
// it: what each entry of its journal is, and where the undo cursor stands.
// The answer to "what was done to this film?", which reviewing a series of
// delegated montages asks. Only what the journal keeps: it keeps no date
// and no author, so neither is here.
struct HistoryEntry {
    // Place in the history, from 1; entries 1 to `applied` are in effect.
    size_t rank = 0;
    // The operation's type as the journal spells it ("AddMarker"), or
    // "Batch" for a group.
    std::string type;
    // A batch's label; empty when it has none, and for one operation.
    std::string label;
    // The operations the entry applies in one step: a batch's members, else
    // 1.
    size_t operations = 1;
    // DeleteGap is the journal's name for every edit that writes the
    // resulting contents of tracks: a gap closed, but also a rush inserted
    // or overwritten from the Source monitor and a range deleted
    // (TimelineSourceEditOperation, TimelineRangeDeleteOperation) -- and
    // even a gap closed is journaled with its tracks' contents
    // (ApplyDeleteGap), so their presence tells nothing. The clips that
    // appear and disappear between the entry's two states do: a gap closed
    // adds and removes none. Counted by ID, on DeleteGap entries only
    // (`counts_clips`).
    bool counts_clips = false;
    size_t clips_added = 0;
    size_t clips_removed = 0;
    // Past the undo cursor: undone, and what Redo would replay, in order.
    bool undone = false;
    // MOTIF1 -- the reasons the entry's motifs give, distinct, in the order
    // they cover its members; list_motifs says the rest.
    std::vector<std::string> reasons;
};

struct TimelineHistory {
    Ulid timeline_id;
    // Entries in the journal, undone ones included.
    size_t total = 0;
    // Entries in effect: the undo cursor.
    size_t applied = 0;
    // The last entries, oldest first: at most the limit asked for.
    std::vector<HistoryEntry> entries;
};

// An old project's journal must not make describe megabytes.
inline constexpr size_t kHistoryEntryLimit = 200;

TimelineHistory SummarizeHistory(const Ulid& timelineId, const EditLog& log,
                                 size_t limit = kHistoryEntryLimit);
// describe's `history`, as JSON, the same on every surface.
std::string TimelineHistoryJson(const TimelineHistory& history);

// PA9 -- docs/roadmap/PARITE-2026-11.md. What a timeline became since a
// state someone holds. An agent keeps a model of the timeline and patches
// it with each edit's delta; between two of its turns the person edits in
// the window, and the agent's model is then wrong without a word. Every
// edit result names the state it leaves by its `doc_hash`: this finds that
// state in the journal and answers the one delta from it to now, with the
// entries in between -- the person's edits and the agent's own alike, the
// journal keeping no author.
//
// The state is looked for backwards first (undoing a copy, entry by
// entry), then forwards (redoing it), which is where it is when the person
// undid past it. A state the journal no longer leads to -- undone, then
// new edits made -- is not found, and that is an answer, not an error:
// the caller reads the timeline again.
struct TimelineChanges {
    Ulid timeline_id;
    // The state asked about, and the one the timeline is in.
    std::string since;
    std::string doc_hash;
    bool found = false;
    // True when the state asked about lies ahead of the undo cursor: the
    // entries between were undone, not applied.
    bool undone = false;
    // Whether the two states differ, and what a delta says of it: clips,
    // tracks, and the markers, styles, transitions and compositions created
    // or removed. A change it has no word for -- a marker renamed, a style's
    // settings, a track locked -- leaves it empty while `changed` is true;
    // the entries then say what kind of edit to read again.
    bool changed = false;
    DocumentDelta delta;
    // The journal's entries between the two states, oldest first.
    std::vector<HistoryEntry> entries;
};

// How far back, and how far forward, a state is looked for.
inline constexpr size_t kChangesSearchLimit = 500;

// False only when a copy of the journal cannot be undone or redone, which
// would be a broken journal; a state not found is `found: false`.
bool ReadTimelineChanges(const Document& current, const EditLog& log,
                         const std::string& sinceHash, TimelineChanges& changes,
                         std::string& message);

// list_changes' answer, the same on every surface.
std::string TimelineChangesJson(const TimelineChanges& changes);

struct ProjectEntry {
    ProjectOperation op;
    ProjectOperation inverse;
    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. Why the project was edited:
    // a timeline created, cloned, declined or chosen (EditMotif.h). A
    // project entry is one member -- a B10 batch is recorded as one change
    // -- so at most one motif, spelled as a timeline entry's are; empty when
    // nobody said. Trace, not state, as on a timeline's journal.
    std::vector<EntryMotif> motifs;
};

// MOTIF6 -- the project journal's versions, under MOTIF1's policy: without
// a motif, version 1, byte for byte what every build before MOTIF6 wrote
// and reads; with one, version 2, which those builds refuse.
inline constexpr int kProjectEditLogVersion = 1;
inline constexpr int kProjectEditLogVersionWithMotifs = 2;

class ProjectEditLog {
public:
    // MOTIF6 -- `motifs` ride on the entry (ProjectEntryMotifsFor builds
    // them); a span other than the entry's one member, or an invalid motif,
    // refuses the edit before it is applied.
    bool Apply(Project& project, ProjectOperation operation, EditError& error,
               std::string& message, std::vector<EntryMotif> motifs = {});
    bool ApplyBatch(Project& project, std::vector<ProjectOperation> operations,
                    EditError& error, std::string& message,
                    std::vector<EntryMotif> motifs = {});
    bool Undo(Project& project, EditError& error, std::string& message);
    bool Redo(Project& project, EditError& error, std::string& message);

    size_t AppliedCount() const;
    size_t UndoneCount() const;
    const std::vector<ProjectEntry>& AppliedEntries() const;
    const std::vector<ProjectEntry>& UndoneEntries() const;
    // DEC24 -- SAVING_ROADMAP.md. How many entries written before S6 (two
    // whole projects each) Deserialize read as deltas
    // (CompactLegacyProjectEntry). Serialize writes them in that form: a
    // journal read with a count above zero is smaller written back, and
    // reads faster (LoadProjectEditLogMigrating, ProjectStorage.h).
    size_t CompactedOnRead() const;

    bool Save(const std::string& path, EditError& error,
              std::string& message) const;
    static bool Load(const std::string& path, ProjectEditLog& output,
                     EditError& error, std::string& message);
    std::string Serialize() const;
    static bool Deserialize(const std::string& json, ProjectEditLog& output,
                            EditError& error, std::string& message);

private:
    std::vector<ProjectEntry> applied_;
    std::vector<ProjectEntry> undone_;
    size_t compacted_on_read_ = 0;
};
