#pragma once

// F1.5 -- ROADMAP.md. Multicam groups from the command line, before MCP and
// the application; `--apply-op` alone made a caller spell every angle id and
// could not say which angle a clip belongs to.
//
//   --add-multicam-group <projet> <plan,plan[,...]> [--name <nom>]
//       [--angle-name <nom>]... [--active <plan>] [--timeline <id>]
//   --set-multicam-angle <projet> <angle|plan|none> [--group <groupe>]
//       [--timeline <id>]
//   --remove-multicam-group <projet> <groupe> [--timeline <id>]
//   --list-multicam-groups <projet> [--timeline <id>]
//   --split-multicam-group <projet> <groupe> <position> [--active <angle>]
//       [--timeline <id>]
//
// Every identifier is resolved by IdResolver exactly as the MCP tools resolve
// theirs: a full ULID, or an unambiguous prefix, never a guess. The timeline
// defaults to the active one. `--angle-name` is repeated once per clip, in
// order. `none` clears the group's active angle and needs `--group`. The
// write commands hold the project's session lock (IA7) and apply one
// reversible operation through ApplyOperationCommand, the `--apply-op` path;
// their result adds what DescribeMulticamEdit reports (Multicam.h).

#include <string>
#include <vector>

bool IsMulticamCommand(const std::string& command);

// `command` is one of the four flags above; `arguments` follow it.
int MulticamCommand(const std::string& command,
                    const std::vector<std::string>& arguments,
                    std::string& output);
