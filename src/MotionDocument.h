#pragma once

// MOTION-2026-09 M3 -- ROADMAP.md, docs/MOTION_FORMAT.md. Compositions in the
// document: what makes a stored composition valid, the identities it
// carries, the edits the motion operations make to its tree, and the JSON
// every surface reads it back as.
//
// A stored composition is the motion format itself (Document.h says why),
// so every edit here is an edit of that tree: find the node a ULID names,
// rebuild the path down to it with the change, and let the motion parser
// judge the result. Paths inside the tree are positional, and never leave
// this file: what an operation carries, what the journal records and what a
// caller names is always a ULID (PHILOSOPHY.md principle 5).
//
// Portable: no framework, no file. The CLI and MCP surfaces read a
// composition through MotionCompositionJson, so both say the same bytes.

#include "Document.h"
#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "Ulid.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Where a value sits in a definition: an object key or an array index at
// each step. Internal to navigation, never persisted.
struct MotionTreeStep {
    std::string key;
    size_t index = 0;
    bool is_index = false;
};
using MotionTreePath = std::vector<MotionTreeStep>;

const mcp_json::Value* MotionTreeAt(const mcp_json::Value& root,
                                    const MotionTreePath& path);

// `root` rebuilt with the value at `path` replaced, or its key removed when
// `replacement` is empty. An object keeps its key order; a key it did not
// have is appended.
mcp_json::Value MotionTreeWith(
    const mcp_json::Value& root, const MotionTreePath& path,
    const std::optional<mcp_json::Value>& replacement);

// `root` rebuilt with `value` inserted in the array at `arrayPath` before
// position `before` (appended when `before` is past the end).
mcp_json::Value MotionTreeInsert(const mcp_json::Value& root,
                                 const MotionTreePath& arrayPath, size_t before,
                                 const mcp_json::Value& value);

// `root` rebuilt without the array element at `path`.
mcp_json::Value MotionTreeErase(const mcp_json::Value& root,
                                const MotionTreePath& path);

// A node of a definition: the root, a child, or a repeat's template.
struct MotionNodeLocation {
    MotionTreePath path;
    // The node that holds it among its `children`, or the repeat whose
    // template it is; empty path for the root.
    MotionTreePath parent;
    bool is_root = false;
    bool is_template = false;
};

bool FindMotionNode(const mcp_json::Value& definition, const Ulid& uid,
                    MotionNodeLocation& location);

// A node named by the author's `id` (unique in a composition, M1).
bool FindMotionNodeByName(const mcp_json::Value& definition,
                          const std::string& name,
                          MotionNodeLocation& location);

// The interpolation holding the keyframe `uid`, and its position there.
struct MotionKeyframeLocation {
    MotionTreePath interpolate;
    size_t index = 0;
};

bool FindMotionKeyframe(const mcp_json::Value& definition, const Ulid& uid,
                        MotionKeyframeLocation& location);

// Gives a fresh ULID to every node of `node`'s subtree that has no `uid`,
// and fresh `uids` to every interpolation that names none. Identities
// already there are kept as written: the parser, not this, judges them.
mcp_json::Value WithMotionIdentities(const mcp_json::Value& node);

// The same for a whole definition, from its root node down.
mcp_json::Value WithDefinitionIdentities(const mcp_json::Value& definition);

// The same for one animation (`{"interpolate": ...}` or `{"spring": ...}`).
mcp_json::Value WithAnimationIdentities(const mcp_json::Value& animation);

// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The inverse, for a
// definition that leaves its document: every node's `uid` and every
// interpolation's `uids` removed, from the root node down. A piece of the
// motion library is added to many projects, and each addition must draw its
// own identities.
mcp_json::Value WithoutDefinitionIdentities(const mcp_json::Value& definition);

// Every identity a composition holds, in tree order: its own id, then each
// node's uid and each keyframe's. They share the document's one ID space.
std::vector<Ulid> CompositionIdentities(const DocumentComposition& composition);

// The node uids and keyframe uids of a definition, apart.
void MotionDefinitionIdentities(const mcp_json::Value& definition,
                                std::vector<Ulid>& nodeUids,
                                std::vector<Ulid>& keyframeUids);

// MOTION-2026-09 M6 -- `definition` with the entry `name` of `section`
// ("variants" or "formats") set to `value`, or removed when `value` is
// empty. A section appears, before `root`, with its first entry and goes with
// its last. False, leaving `output` alone, when removing what is not there.
bool MotionDefinitionWithSetEntry(const mcp_json::Value& definition,
                                  const std::string& section,
                                  const std::string& name,
                                  const std::optional<mcp_json::Value>& value,
                                  mcp_json::Value& output);

// A stored composition as the evaluator reads it.
bool ParseDocumentComposition(const DocumentComposition& composition,
                              MotionComposition& parsed, MotionError& error);

// REMOTION-PARITY-2026-09 REM2 -- what a definition names beyond itself,
// literally: the project media of its video and audio nodes, and the
// compositions its instances show. A bound `media` is resolved only when a
// frame is evaluated, and is not listed.
void MotionDefinitionReferences(const mcp_json::Value& definition,
                                std::vector<Ulid>& media,
                                std::vector<Ulid>& compositions);

// REM2 -- the compositions `id` shows through its instances, transitively,
// itself excluded, in the order first reached. Unknown ids are skipped: the
// document refuses them (ValidateCompositionReferences).
std::vector<const DocumentComposition*> CompositionDependencies(
    const std::vector<DocumentComposition>& compositions, const Ulid& id);

// REM2 -- `id`'s dependencies, parsed, as an evaluation of it reads them.
bool BuildMotionLibrary(const std::vector<DocumentComposition>& compositions,
                        const Ulid& id, MotionLibrary& library,
                        MotionError& error);

// REM2 -- every media of the document, by ULID, to its path as stored
// (relative to the project, or absolute): what a `media` node plays.
std::map<std::string, std::string> MotionMediaPaths(const Document& document);

// REM2 -- a text that changes whenever what composition `id` reaches
// changes: the definitions its instances show, and the paths of the media
// it plays (every media of the project when one is named by a binding,
// since any of them could be). Joined to a picture key, it keeps a cached
// frame from outliving an edit of a precomposition or a relink.
std::string MotionReachKey(const Document& document, const Ulid& id);

// REM2 -- what an evaluation of composition `id` of `document` reaches: its
// dependencies, parsed, and the document's media.
bool MotionReachFor(const Document& document, const Ulid& id,
                    MotionReach& reach, MotionError& error);

// REM2 -- the same from a project's parts, for a surface that holds a
// Project rather than one timeline's Document.
bool MotionReachFor(const std::vector<DocumentComposition>& compositions,
                    const std::vector<DocumentSource>& sources,
                    const std::vector<LibraryMedia>& library, const Ulid& id,
                    MotionReach& reach, MotionError& error);

// REM2 -- Document::Validate's part across compositions: every media a
// definition names is one of the project's (UnknownMedia), every instance
// names a composition of the project (UnknownComposition) with values its
// props accept (UnknownProp, PropTypeMismatch, PropOutOfRange), and no
// composition shows itself through its instances (CompositionCycle).
// `mediaIds` are the document's media and sources.
bool ValidateCompositionReferences(
    const std::vector<DocumentComposition>& compositions,
    const std::set<Ulid>& mediaIds, MotionError& error);

// Document::Validate's part: a name, a definition the motion parser accepts,
// and an identity on every node and every keyframe. The error names the
// motion refusal's code and path.
bool ValidateDocumentComposition(const DocumentComposition& composition,
                                 MotionError& error);

// The composition as the CLI and MCP give it back: id, name and the stored
// definition, verbatim.
std::string MotionCompositionJson(const DocumentComposition& composition);

// One line per composition: id, name, size, cadence, duration and its props
// with their types -- what an agent needs to choose one before reading it.
std::string MotionCompositionListJson(
    const std::vector<DocumentComposition>& compositions);
