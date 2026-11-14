#pragma once

// LG1 -- docs/roadmap/LEGENDES-2026-10.md. The macOS side of a carton: Core
// Text measures its lines and traces its glyphs where CaptionLayout.h puts
// them. Installed for every surface of the model by the executable, next to
// the motion renderer (InstallMotionFrameRenderer, MotionCli.h).

// Makes CurrentCaptionRenderer() (CaptionRenderer.h) answer.
void InstallCaptionRenderer();
