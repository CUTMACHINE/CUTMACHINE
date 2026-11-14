#pragma once

// PRD14 -- ROADMAP.md. What a tool accepts beyond what its schema publishes.
//
// An agent laying a first clip without the skill at hand was refused four
// times running. Two of those refusals carried no meaning: a rush is
// `source_id` in three tools and `media_id` in some twenty-five others, for
// one and the same identifier, and fourteen enumerations are capitalised
// (`Insert`, `Head`, `Frames`) where twenty-eight are not (`hevc`, `cut`).
// Neither can be guessed, and renaming either would refuse tomorrow what
// every skill, script and agent session sends today. So the names and the
// spellings stay as they are, and the catalog reads the other ones too.
//
// The rules are read from each tool's own inputSchema, never listed per
// tool: an argument the schema declares under one name of a rush takes the
// other name, and a value the schema declares as an enumeration is matched
// without regard to case and handed on in the schema's own spelling. A tool
// added tomorrow is covered by declaring its arguments. The dispatchers
// never see a synonym or a foreign case, so their own checks and refusals
// keep naming the canonical spelling.
//
// Case is the only latitude an enumeration gets: `start_at_cut` is not
// `StartAtCut`. Each tolerance is a promise to keep, so none is made that
// nobody asked for.
//
// Operations themselves (`--apply-op`, the journal) are not read through
// this: their format is frozen by fixtures.

#include "Json.h"

#include <memory>
#include <string>

class McpArgumentRules {
public:
    // No rule: Normalize hands its input back unchanged.
    McpArgumentRules() = default;

    // Reads the rules out of a tool's inputSchema, at every depth reached
    // through `properties` and `items`. A schema that does not parse yields
    // no rule. Alternatives (`oneOf`, `anyOf`) are not entered: the catalog
    // declares no enumeration and no rush under one, and
    // tests/mcp_tools_tests.cc fails the day it does.
    explicit McpArgumentRules(const std::string& inputSchemaJson);

    bool Empty() const { return root_ == nullptr; }

    // `arguments` as the tool's dispatcher expects them: each rush under the
    // name the schema declares, each enumeration value in the schema's
    // spelling, everything else as it came, in the caller's order. A value
    // that matches no spelling is left alone for the dispatcher to refuse in
    // its own words.
    //
    // False, with `message` naming both arguments, when a rush is given
    // under its two names with two different values: which one was meant is
    // not for the engine to guess. The same value twice is one argument.
    bool Normalize(const std::string& toolName,
                   const mcp_json::Value& arguments,
                   mcp_json::Value& normalized, std::string& message) const;

private:
    struct Node;
    std::shared_ptr<const Node> root_;
};

// The other name of a rush argument (`media_id` for `source_id`, `source_ids`
// for `media_ids`, and back), or nullptr when `name` is not one. A montable
// DocumentSource carries the ULID of its library media, so the two names
// take the same values: a full ID, an unambiguous prefix, an alias like M1.
const char* RushArgumentTwin(const std::string& name);
