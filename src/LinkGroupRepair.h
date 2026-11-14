#pragma once

#include "Document.h"
#include "Operations.h"

#include <cstddef>
#include <functional>
#include <vector>

// Out of lot -- the author, 4 November 2026: « J'ai encore le problème des
// blocs fusionnés que je peux pas bouger ». A link group binds a picture and
// its sounds where they lie together. Before 2a84c38 (#213, 30 October 2026) a
// cut kept one group for every stretch it left of a block, so one click in
// the window took the whole block, and a drag or a trim carried every
// stretch with it. Timelines cut before then still hold those groups; four
// projects had been repaired by hand, one pair at a time, before this.
//
// The rule: the members of a group that overlap in time belong together --
// a picture and its sound, a J or L cut, a recorder's sound beside the
// camera's (SeparateSound.h). Members that only touch, or lie apart, do not.
// Each such place gets a group of its own; the earliest keeps the old one.
// Every member of a place is linked again to the picture of that place
// (SetClipLinkOperation), which sets its sync reference against its own
// pair, the offset of a slipped sound included. A clip alone at its place
// has no partner there and loses its link (two at a time: clearing a link
// names two clips; an odd one keeps the old group, alone in it). A place
// holding a freeze or a speed ramp, which a link refuses, keeps the old
// group, and every other place then takes a new one.
//
// PlanLinkGroupRepair only reads. The window applies its operations as one
// undo step when it opens a timeline; split_link_groups applies them for an
// agent. Neither mutates the document outside the journal.
struct LinkGroupRepair {
    std::vector<SetClipLinkOperation> operations;
    // Groups that bound clips at several places.
    size_t groups = 0;
    // Places given a group of their own (the earliest of each group keeps
    // the old one and is not counted).
    size_t places = 0;
    // Clips alone at their place whose link was cleared.
    size_t cleared = 0;
    // Clips left in their old group: at a place a link refuses, or the odd
    // one of those alone at their place.
    size_t left_alone = 0;

    bool Empty() const { return operations.empty(); }
};

// `newGroupId` names each group a place receives; GenerateUlid in the
// window and the tools, a fixed sequence in tests.
LinkGroupRepair PlanLinkGroupRepair(const Document& document,
                                    const std::function<Ulid()>& newGroupId);

// The repair as batches the journal accepts, in order: one undo step each,
// kMaxBatchOperations links at most. Empty when there is nothing to repair.
std::vector<BatchOperation> LinkGroupRepairBatches(
    const LinkGroupRepair& repair);
