#pragma once

// GENERATIF-2026-10 GN2 -- docs/roadmap/GENERATIF-2026-10.md. The two tools
// of paid generation: propose_generation, which spends nothing and says what
// would be sent, to which model, at what announced cost; generate_media,
// which spends, and only on a proposal of this session that the person
// accepted. Kept apart from McpTools.cc, which registers them, so that the
// rule of paid generation reads in one place (Generation.h).

#include "Generation.h"
#include "McpMotionTools.h"

void RegisterGenerationTools(const McpToolAdder& add,
                             generation::GenerationProposalStore& proposals);
