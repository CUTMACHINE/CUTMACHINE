#pragma once

// The MCP tool catalog (ROADMAP.md F1.2): one entry per operation in
// Operations.h that a human can already trigger through the app or the
// `--apply-op` CLI path, plus `describe`/`undo`/`redo`. Excluded on purpose:
//   - JoinClip: not a human-triggerable gesture anywhere in the app: it is
//     only ever produced as SplitClip's stored inverse, and constructing one
//     by hand requires supplying the exact pre-split clip extents. Its
//     effect is already reachable through the `undo` tool.
//   - AddMulticamGroup / SetMulticamActiveAngle: Operations.cc rejects both
//     with EditError::InvalidOperation pending ROADMAP.md ticket F1.5; a
//     tool that always fails would not be a real capability.
//
// Every dispatch function here validates its JSON arguments (rejecting
// unknown keys and non-finite numbers, naming the offending field),
// resolves short IDs via IdResolver, constructs the matching Operations.h
// struct, and hands it to McpBackend -- never mutates a Document itself.
//
// PRD14 -- ROADMAP.md. What a dispatch function validates is what the
// catalog normalised first (McpArguments.h): a rush under either of its
// names and an enumeration value in any case reach it under the name and
// in the spelling its schema declares.

#include "EditPlan.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpArguments.h"
#include "McpBackend.h"
#include "McpToolProfiles.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace generation {
class GenerationProposalStore;
}

// One tool's JSON-RPC-facing dispatch: given the backend, an ID resolver
// already built over the backend's current document, and the tool's
// "arguments" object, either applies an operation and reports success, or
// fails with errorName/message naming what was wrong. Never throws.
using McpDispatchFn = std::function<bool(
    McpBackend& backend, const IdResolver& resolver,
    const mcp_json::Value& arguments, std::string& resultJson,
    std::string& errorName, std::string& message)>;

// PRD15 -- ROADMAP.md. What a tool does to what lies around it, read from
// what its code does rather than from its name: a client tells a reading
// from an edit, and an edit that undo takes back from one it does not, by
// the protocol's annotations, which are derived from this and from nothing
// else (McpToolAnnotationsJson). Declared once per tool, by name, in the
// registry's table; a tool without one is Unclassified, which
// tests/mcp_tools_tests.cc refuses.
enum class McpToolClass {
    Unclassified,
    // Changes no project, no file and no cache.
    Read,
    // Changes only what this server holds for its session: a plan, a task,
    // the project it has open. Nothing on disk but a session lock.
    Session,
    // Leaves the edit alone and writes a cache beside the project
    // (transcripts, envelopes, picture quality, beats).
    Analysis,
    // An operation of the timeline's journal: the `undo` tool takes it back.
    JournaledEdit,
    // An operation of the project's journal (timelines added, removed,
    // renamed, activated). Reversible by `--undo-project-op` and in the app;
    // the `undo` tool, which walks the timeline's journal, does not reach it.
    ProjectEdit,
    // Writes a file at a path the caller names, and may replace one.
    FileWrite,
    // Changes the project or the disk outside any journal: nothing undoes it.
    Unjournaled,
    // Reads another program or a server, and changes nothing there.
    ExternalRead,
    // Acts on another program or a server.
    External,
};

// The class's name as the catalog's tests and reports spell it.
const char* McpToolClassName(McpToolClass toolClass);

// Every name the registry's class table declares, so a test can tell a
// stale entry from a tool left without a class.
std::vector<std::string> McpToolClassTableNames();

// Metadata for one MCP tool, as surfaced by `tools/list`.
struct McpTool {
    std::string name;
    std::string description;
    // Raw JSON text of the tool's JSON Schema `inputSchema` object.
    std::string input_schema_json;
    // IA5 -- ROADMAP.md. What a running server (McpSession.h) offers around
    // this tool; its schema carries the matching arguments. `background`: an
    // analysis, or since PRD16 an export, that may run as a task
    // (`async`). `idempotent`: a call that
    // changes something, and so accepts `idempotency_key` and
    // `expected_revision` -- not the protocol's `idempotentHint`, which
    // says nearly the opposite. `writes_analysis_cache`: an analysis that
    // installs a project cache file, which a background task may be
    // rewriting. PRD15 -- the class says no less than these three, and
    // tests/mcp_tools_tests.cc checks that it never contradicts them.
    bool background = false;
    bool idempotent = false;
    bool writes_analysis_cache = false;
    McpToolClass tool_class = McpToolClass::Unclassified;
    // PROFIL-2026-11 PF1 -- the domain whose file registers the tool
    // (McpToolProfiles.h), which a profile serves whole or not. Not
    // published: a client has no use for it.
    std::string domain;
};

// PRD17 -- ROADMAP.md. Strict timeline mode (McpBackend::
// RequiresExplicitTimeline): the tools that act on one timeline and would
// otherwise take the active one, and the refusal, which names the project's
// timelines. McpToolRegistry::Call applies it; a server's session applies
// it too before it queues a task.
bool McpToolTargetsOneTimeline(const std::string& name);
std::string McpTimelineRequiredMessage(McpBackend& backend,
                                       const std::string& tool);
// PRD40 -- ROADMAP.md. The one rule strict mode applies, wherever it is
// checked: the call targets one timeline and does not name it -- unless it
// names only a rush (read_frame with media_id and no clip_id), which reads
// no timeline. `arguments` as the catalog normalizes them (McpArguments.h).
bool McpCallNeedsTimeline(const std::string& name,
                          const mcp_json::Value& arguments);

// PRD15 -- the tool's `annotations` object, in the MCP specification's
// vocabulary (ToolAnnotations), holding only the hints whose value differs
// from the protocol's default: readOnlyHint false, destructiveHint true,
// idempotentHint false, openWorldHint true.
std::string McpToolAnnotationsJson(McpToolClass toolClass);

// PRD15 -- one tool as `tools/list` and `--tools` publish it: name,
// description, inputSchema and annotations. Both go through this, so the two
// listings cannot drift apart. The chat panel builds its own list for the
// model (ChatLlmClient.cc) and sends no annotations, which a model has no use
// for.
std::string McpToolListingJson(const McpTool& tool);

// The `tools` array of a listing: every tool, in catalog order.
std::string McpToolsListingJson(const std::vector<McpTool>& tools);

// IA5 -- the arguments a running server consumes before the tool sees them.
// Without a server (`--call-tool`, the chat panel) they are refused by name:
// a task or a replay nobody keeps would be a promise broken in silence.
inline constexpr char kMcpAsyncKey[] = "async";
inline constexpr char kMcpIdempotencyKey[] = "idempotency_key";
inline constexpr char kMcpExpectedRevisionKey[] = "expected_revision";
// MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. Why an edit is made: read by the
// catalog itself (McpToolRegistry::Call), never by a dispatcher, and kept
// in the journal beside the entry the call writes (EditMotif.h).
inline constexpr char kMcpMotifKey[] = "motif";

struct McpToolCallOutcome {
    bool ok = false;
    // B2 -- ROADMAP.md. Always a JSON object: the tool payload on success,
    // or {"ok":false,"error":...,"detail":...} on refusal. The named
    // fields below remain available to callers that need branching without
    // parsing the payload.
    std::string result_json;
    std::string error_name;
    std::string message;
    // A tool may return a picture as well as text (today only read_frame).
    // Base64, with its MIME type; empty when the tool returned text only.
    //
    // Carried out of the dispatcher through two reserved keys in its result
    // JSON rather than by widening McpDispatchFn: the signature is shared by
    // every tool in the catalog, and changing it to thread an out-parameter
    // no other tool populates would touch all of them to serve one. Call()
    // lifts the keys into these fields and removes them from the text, so
    // nothing downstream sees the convention.
    std::string image_base64;
    std::string image_mime;
};

// The reserved envelope a dispatcher returns when it has a picture: the two
// image fields plus the result text it would otherwise have returned on its
// own. An envelope rather than extra keys on the real payload, so lifting it
// is a lookup instead of a rebuild -- mcp_json::Value has no key
// enumeration, and widening a shared type to serve one tool would be the
// wrong trade.
inline constexpr char kMcpImageDataKey[] = "__image_base64";
inline constexpr char kMcpImageMimeKey[] = "__image_mime";
inline constexpr char kMcpImageTextKey[] = "__text";

class McpToolRegistry {
public:
    // PROFIL-2026-11 PF1 -- the tools `profile` serves (McpToolProfiles.h):
    // every one by default. A tool it leaves out is in neither Tools() nor
    // the steps of a plan.
    explicit McpToolRegistry(
        const McpToolProfile& profile = FullMcpToolProfile());

    const std::vector<McpTool>& Tools() const { return tools_; }
    const McpToolProfile& Profile() const { return *profile_; }
    // The whole catalog's size, whatever the profile serves of it.
    size_t CatalogSize() const { return catalog_size_; }
    // Whether Tools() holds `toolName`.
    bool Offers(const std::string& toolName) const;

    // Looks up `toolName` and, if found, snapshots the backend's current
    // document to build an IdResolver and runs the tool's dispatch
    // function. Returns an outcome with error_name "UnknownTool" if no such
    // tool is registered, and "ToolNotInProfile" if the catalog has it but
    // the profile left it out.
    McpToolCallOutcome Call(McpBackend& backend, const std::string& toolName,
                            const mcp_json::Value& arguments) const;

    // PRD14 -- ROADMAP.md. `arguments` as `toolName`'s dispatcher receives
    // them (McpArguments.h): every dispatch function of this catalog is
    // given its arguments through this, whoever calls it -- Call, and the
    // steps of a plan. Exposed for a caller that reads arguments before the
    // catalog does, so it reads the same ones; applying it twice changes
    // nothing. False, with `message`, when a rush is named twice with two
    // values. An unknown tool has no rule: its arguments come back as given.
    bool NormalizeArguments(const std::string& toolName,
                            const mcp_json::Value& arguments,
                            mcp_json::Value& normalized,
                            std::string& message) const;

    // PL3 -- ROADMAP.md. The plans prepare_edit_plan keeps for this catalog's
    // session until apply_edit_plan or discard_edit_plan (EditPlan.h). One
    // catalog serves one session -- a server, a chat panel, one CLI command
    // -- so the store lives with it; the CLI reaches it directly to write a
    // plan to the file its caller names and to read one back.
    EditPlanStore& EditPlans() const { return edit_plans_; }

private:
    // PRD35 -- ROADMAP.md. What Call does; Call adds the engine log's line
    // around it (EngineLog.h).
    McpToolCallOutcome CallTool(McpBackend& backend,
                                const std::string& toolName,
                                const mcp_json::Value& arguments) const;

    std::vector<McpTool> tools_;
    std::vector<McpDispatchFn> dispatch_;  // index-aligned with tools_
    // PRD14 -- read from each tool's schema; index-aligned with tools_.
    std::vector<McpArgumentRules> argument_rules_;
    // PF1 -- the profile, one of McpToolProfiles()' (static storage), the
    // catalog's size before it chose, and the names it left out.
    const McpToolProfile* profile_;
    size_t catalog_size_ = 0;
    std::vector<std::string> withheld_;
    mutable EditPlanStore edit_plans_;
    // GENERATIF-2026-10 GN2 -- the generations proposed in this session
    // (Generation.h), spent by generate_media.
    std::shared_ptr<generation::GenerationProposalStore> generation_proposals_;
};
