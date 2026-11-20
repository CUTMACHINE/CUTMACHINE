#pragma once

// The envelope every headless surface answers in: {"ok":true,...} or
// {"ok":false,"error":...,"detail":...,"result":...}, and its reader.
//
// Split out of Cli.h: an importer or a store that refuses in this shape
// needs the envelope, not the command line, and including Cli.h for it made
// the model depend on one of its surfaces (PHILOSOPHY.md, principle 3).

#include <string>

// B2 -- ROADMAP.md. Every headless surface uses this serializer for a
// refusal, including commands implemented outside Cli.cc. Keeping the exit
// status beside the JSON construction prevents a caller from receiving a
// non-zero status with a success-shaped or free-form payload.
int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus = 1);
// BENCH-2026-09 (B3) -- the same refusal with a machine-readable `result`
// object, the shape McpToolRegistry::Call gives a dispatcher's partial
// result, so a refusal that names what to do reads the same on both
// surfaces. An empty or non-object `resultJson` is left out.
int FailCliCommandWithResult(const std::string& errorCode,
                             const std::string& detail,
                             const std::string& resultJson, std::string& output,
                             int exitStatus = 1);

// The reader of that envelope, for a backend that runs a command and reports
// through McpBackend: on success `resultJson` is the whole output; on failure
// `errorName` and `message` are its `error` and `detail`, and `resultJson`
// its `result` object when the refusal carries one (PRD20). CHAT-PARITE --
// the project and live backends share it, so a refusal reads the same from
// the MCP server and from the chat panel.
bool ReadCliCommandResult(const std::string& output, bool commandSucceeded,
                          std::string& resultJson, std::string& errorName,
                          std::string& message);
// PRD20 -- ROADMAP.md. A refusal of the command line itself (ValidationFailed
// with exit status 2) says how the command is called: `usage`, a
// CliUsageLine (CliCommands.h), is appended to its detail unless the detail
// already holds one. Any other output is returned as it is. For the commands
// that read their own arguments outside main() -- motion, multicam -- whose
// refusals main() relays without having written them.
std::string WithCliUsage(const std::string& output, int exitStatus,
                         const std::string& usage);
// PRD39 -- ROADMAP.md. `--help` promises that every command answers
// {"ok":true,...} or {"ok":false,...}, and --describe,
// --shot-quality-report, --isolated-frames and --locate-source-frame
// printed their report bare. This puts "ok":true in front of a JSON object
// that does not start with "ok", and changes nothing else: neither a byte
// of the report nor an answer that already carries "ok", or is no object.
std::string WithCliOk(const std::string& output);
