#pragma once

// MOTION-2026-09 M3 -- ROADMAP.md. How the motion operations of Operations.h
// change a document's compositions. Kept beside the engine rather than in
// Operations.cc so that the edits of a composition's tree read in one place;
// ApplyOperation dispatches here like to any other operation, on the same
// candidate copy, with the same all-or-nothing contract.

#include "MotionComposition.h"
#include "Operations.h"

#include <string>

bool IsMotionOperation(const Operation& operation);

// Applies a motion operation to `candidate` and sets `inverse`. The
// operation is enriched with the identities first application gives. On
// failure `candidate` may be partly changed: the caller discards it.
bool ApplyMotionOperation(Document& candidate, Operation& operation,
                          Operation& inverse, EditError& error,
                          std::string& message);

// A refusal of the motion parser as an operation refusal: the three prop
// codes keep their names, everything else is InvalidMotion. The message
// starts with the parser's own code and path.
void MotionRefusal(const MotionError& motion, EditError& error,
                   std::string& message);
