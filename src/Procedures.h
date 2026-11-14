#pragma once

// PRD18 -- ROADMAP.md (ACP4, docs/roadmap/ACP-2026-09.md). The editing
// procedures the engine carries: the repository's skills
// (.claude/skills/<name>/), embedded in the binary when it is built
// (cmake/EmbedProcedures.cmake).
//
// A skill says how to edit an interview or make motion design with these
// tools: as much the product as the engine is. Claude Code loads one only
// when its working directory is the repository, so the app's agent panel,
// Codex, or any agent that reaches the engine over ACP or MCP never had
// them. Embedded, they travel with the binary the way the server's
// instructions do, and reach every surface from here: `get_procedure`, the
// tool a model calls on its own (McpTools.cc); one MCP prompt per procedure,
// for a person who starts one by hand (McpServer.cc); `--procedure` on the
// command line (Cli.h). The files under .claude/skills stay the source, and
// Claude Code in the repository still loads them natively.
//
// Plain C++: no AppKit, and no file is opened -- the bytes are compiled in.

#include "Json.h"

#include <cstddef>
#include <string>
#include <vector>

// One file of a skill as the build embedded it, byte for byte.
struct EmbeddedProcedureFile {
    const char* procedure;
    // Relative to the procedure's directory, '/'-separated, as the
    // procedure's own links write it: "SKILL.md", "references/motifs.md".
    const char* path;
    const unsigned char* bytes;
    size_t size;
};

// The table cmake/EmbedProcedures.cmake writes, by procedure, then by path.
// `count` leaves out the entry that ends it.
const EmbeddedProcedureFile* EmbeddedProcedureFiles(size_t& count);

// The file a procedure is read from when none is named.
extern const char kProcedureMainFile[];

struct ProcedureFile {
    std::string path;
    size_t bytes = 0;
};

struct Procedure {
    // The skill's directory: "monter-une-itw".
    std::string name;
    // The `description` of its SKILL.md front matter: what Claude Code reads
    // to decide when a skill applies.
    std::string description;
    // SKILL.md first, then the others by path.
    std::vector<ProcedureFile> files;
};

// Every procedure the binary carries, by name.
std::vector<Procedure> ListProcedures();

// Where a relative link of a procedure's Markdown leads. A link is written
// relative to the file that holds it: "../exemples/compteur.json" in
// references/motifs.md is exemples/compteur.json, and
// "../monter-une-itw/resolve.md" in a SKILL.md is a file of another
// procedure. Resolved here, so that no reader has to.
struct ProcedureLink {
    // As the text writes it, without its "#anchor".
    std::string link;
    // The procedure it leads to; empty when it leads outside every
    // procedure (a file of the repository, which the binary does not carry)
    // or to nothing.
    std::string procedure;
    // A file of that procedure, or else a folder of it ("exemples").
    std::string file;
    std::string folder;
};

struct ProcedureText {
    // As the procedure spells them, whatever case they were asked in.
    std::string name;
    std::string path;
    // The file's bytes, exactly as the skill holds them.
    std::string text;
    // Every file of the procedure, as ListProcedures gives them.
    std::vector<ProcedureFile> files;
    // The relative links of a Markdown file, once each, in the order the
    // text gives them; none for another kind of file.
    std::vector<ProcedureLink> links;
};

// The file `path` of procedure `name`. `path` is relative to the
// procedure's directory ("mots-et-bornes.md", "outils/planche.py"), as the
// links of its SKILL.md write it; a "./" before it changes nothing. A name
// or a path that matches nothing exactly is taken in another case when
// exactly one matches that way.
//
// False, with `errorName` and a sentence in `message`, otherwise:
//   - UnknownProcedure: no such procedure; the sentence names those there are.
//   - UnknownProcedureFile: no such file; the sentence names the procedure's
//     files, or those of the folder `path` names.
//   - ValidationFailed: `path` is absolute or holds a "..": it leaves the
//     procedure, and nothing outside it is served. When what follows the
//     last ".." is a file of a procedure -- a link copied from a subfolder,
//     or one to another procedure ("../monter-une-itw/resolve.md") -- the
//     sentence gives the name and the file to ask for instead.
bool ReadProcedureFile(const std::string& name, const std::string& path,
                       ProcedureText& file, std::string& errorName,
                       std::string& message);

// A field of the YAML front matter a SKILL.md opens with: a plain, quoted
// or block scalar, its line breaks folded as YAML folds them and the final
// ones dropped. Empty when the text has no front matter or no such field.
std::string SkillFrontMatterField(const std::string& skillText,
                                  const std::string& key);

// What `get_procedure` and `--procedure` answer, in one shape for both.
// Without a name: {"procedures":[{"name","description","files"}]}, each
// file as {"path","bytes"}.
mcp_json::Value ProcedureListingJson();
// With one: {"name","file","files","text"} for the file `path` of procedure
// `name`, and "links" when the text has relative links: each as
// {"link","name","file"} (or "folder" in place of "file") to ask for, or
// {"link","served":false}. False as ReadProcedureFile is.
bool ProcedureFileJson(const std::string& name, const std::string& path,
                       mcp_json::Value& answer, std::string& errorName,
                       std::string& message);
