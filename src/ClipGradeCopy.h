#pragma once
// GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Copying one clip's
// grade onto others, as a colourist copies and pastes a grade on Resolve's
// Color page: the whole stack of the source clip replaces the stack of each
// clip named, in one undo step. Decided by the author on 23 October 2026, and only
// that: neither a LUT per clip nor a gallery of stills.
//
// No new operation: a SetClipEffectsOperation per clip whose stack changes,
// the copies' ids left empty so that the apply names them (each clip keeps
// ids of its own, the document's one space of ids), in a batch when there
// are several. The knobs travel as they are: a color.temperature means the
// same move from each clip's own as-shot balance (ColorEffects.h), as a
// copied temperature does in Resolve.
//
// Policy here, portable; the window's Copy and Paste of a grade and the MCP
// tool paste_clip_grade both build their step by this one function.

#include "Document.h"
#include "Operations.h"

#include <optional>
#include <string>
#include <vector>

struct ClipGradePaste {
    // Nullopt when no clip's stack changes.
    std::optional<Operation> operation;
    // Clips whose stack the step replaces, in the order named.
    std::vector<Ulid> pasted;
    // Clips already graded exactly so (the source itself among them).
    std::vector<Ulid> unchanged;
    // Clips that carry no grade -- a motion clip, a clip off a video track,
    // under colour management a graphic (DEC31) -- with why, in English.
    struct Skipped {
        Ulid clip_id;
        std::string reason;
    };
    std::vector<Skipped> skipped;
};

// Whether `clip` carries a grade in `document`: on a video track, not a
// motion clip, and not a graphic under colour management. Empty when it
// does; the reason, in English, when it does not.
std::string ClipGradeRefusal(const Document& document,
                             const DocumentClip& clip);

// The step that copies `fromClipId`'s stack onto `clipIds`. Fails, naming
// the clip, on an unknown clip or a source that carries no grade; a target
// that carries none is skipped and listed, so that a selection holding a
// title or a sound pastes onto its shots.
bool BuildClipGradePaste(const Document& document, const Ulid& fromClipId,
                         const std::vector<Ulid>& clipIds,
                         ClipGradePaste& paste, std::string& error);

// The same step from a stack copied earlier: what the window holds between
// its Copy and its Paste, so that the source clip may change or go meanwhile,
// as a copied grade does in Resolve. `stack` is pasted as it is.
bool BuildClipGradePasteOfStack(const Document& document,
                                const std::vector<ClipEffect>& stack,
                                const std::vector<Ulid>& clipIds,
                                ClipGradePaste& paste, std::string& error);
