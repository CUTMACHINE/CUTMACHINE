#pragma once

// MOTION-2026-09 M2 -- ROADMAP.md. Motion reaches the CLI before MCP and UI.
//
//   --describe-motion-frame <composition.json> <frame>
//       [--props <json>] [--font-dir <directory>]...
//   --render-motion-frame <composition.json> <frame> <image.png>
//       [--props <json>] [--font-dir <directory>]...
//
// describe prints the evaluated frame (boxes, text lines, baselines) so a
// caller reads geometry instead of measuring pixels; render writes one PNG and
// refuses to overwrite. Relative image sources resolve against the
// composition's directory.
//
// M3 -- a composition of a project, rather than a file:
//
//   --describe-compositions <project> [<composition-id>]
//       the project's compositions, or one of them as it is stored: the same
//       bytes as the MCP tools list_motion_compositions and
//       describe_motion_composition
//   --describe-motion-frame <project> <frame> --composition <id> [...]
//   --render-motion-frame <project> <frame> <image.png> --composition <id>
//       [...]
//       the same commands, reading the composition from the project, whose
//       package relative image sources then resolve against
//
// M6 -- both frame commands take --variant <name> and --format <name>: one of
// the composition's versions, in one of its formats (its size, and the
// props that adapt the card to it). And a whole video, or a batch of them:
//
//   --render-motion-video <composition.json> <output> [--variants <a,b>]
//       [--formats <x,y>] [--alpha] [--overwrite] [--preview]
//       [--font-dir <directory>]...
//   --render-motion-video <project> <output> --composition <id> [...]
//       every version asked for (all declared by default) in every format
//       asked for, frame 0 to the duration at the composition's cadence;
//       "{variant}" and "{format}" in <output> name each file. ProRes 4444
//       with alpha in a .mov with --alpha, H.264 over black otherwise. Every
//       destination is checked and every frame laid out before the first is
//       encoded; the report lists the files completed (MotionVideo.h).
//
// M4 -- what a motion clip of a timeline shows:
//
//   --describe-motion-frame <project> <timeline-frame> --clip <clip-id>
//   --render-motion-frame <project> <timeline-frame> <image.png>
//       --clip <clip-id>
//       <timeline-frame> is a frame of the clip's timeline at its rate; the
//       clip gives the composition, its instance props and the frame of the
//       composition that time falls on (MotionClip.h)

//
// MOTION-AFTER-2026-09 -- the composition for After Effects:
//
//   --export-motion-after <composition.json> <description.json>
//       [--aep <project.aep>] [--name <name>] [--props <json>]
//       [--font-dir <directory>]...
//   --export-motion-after <project> <description.json> --composition <id>
//       [--timeline <id>] [...]
//       writes the after-motion.v1 description (MotionAfterExport.h) that
//       tools/after-bridge/scripts/motion_import.jsx builds a project from,
//       and prints how many compositions, layers and keyframes it holds,
//       with what After gets approximated or not at all

#include <string>
#include <vector>

int DescribeMotionFrameCommand(const std::vector<std::string>& arguments,
                               std::string& output);
int RenderMotionFrameCommand(const std::vector<std::string>& arguments,
                             std::string& output);
int DescribeCompositionsCommand(const std::vector<std::string>& arguments,
                                std::string& output);
int RenderMotionVideoCommand(const std::vector<std::string>& arguments,
                             std::string& output);

// The motion commands above, for main's dispatch.
bool IsMotionCommand(const std::string& name);
int MotionCommand(const std::string& name,
                  const std::vector<std::string>& arguments,
                  std::string& output);

// MOTION-2026-09 M3 -- installs this library's renderer for the surfaces of
// the model that render a frame (MotionFrameRenderer.h): the MCP tool
// render_motion_frame, over MCP, from the CLI's --call-tool and from the
// app's agent. Called once by the executable at startup.
void InstallMotionFrameRenderer();
