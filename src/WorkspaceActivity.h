#pragma once

// ON7 -- docs/roadmap/ONGLETS-2026-10.md. The durable edit history beside
// the markers: show the reason actually recorded, never infer an author or
// an intention from an operation. The window only places the text.

#include "EditLog.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ui::workspace {

inline const char* ReasonTitle(MotifReason reason) {
    switch (reason) {
        case MotifReason::Hesitation:
            return "Hésitation";
        case MotifReason::Pause:
            return "Pause";
        case MotifReason::BoundaryAir:
            return "Raccord de voix";
        case MotifReason::Duplicate:
            return "Doublon";
        case MotifReason::Selection:
            return "Sélection";
        case MotifReason::Cutaway:
            return "Plan de coupe";
        case MotifReason::Rhythm:
            return "Rythme";
        case MotifReason::Brief:
            return "Brief";
        case MotifReason::Marker:
            return "Marqueur";
        case MotifReason::Format:
            return "Format";
        case MotifReason::Captions:
            return "Légendes";
        case MotifReason::Technical:
            return "Correction technique";
        case MotifReason::Other:
            return "Autre";
        case MotifReason::Unstated:
            return "Motif non précisé";
    }
    return "Motif non précisé";
}

inline std::string ChangeText(const HistoryEntry& entry,
                              const std::vector<EntryMotif>& motifs) {
    std::string text = entry.label;
    if (text.empty()) {
        const std::pair<const char*, const char*> titles[] = {
            {"AddClip", "Plan ajouté"},
            {"RemoveClip", "Plan retiré"},
            {"MoveClip", "Plan déplacé"},
            {"TrimClip", "Coupe reprise"},
            {"SplitClip", "Plan coupé"},
            {"SetClipEffects", "Étalonnage"},
            {"SetClipFraming", "Cadrage"},
            {"AddMarker", "Marqueur ajouté"},
            {"UpdateMarker", "Marqueur modifié"},
            {"RemoveMarker", "Marqueur retiré"}};
        for (const auto& title : titles) {
            if (entry.type == title.first) {
                text = title.second;
            }
        }
        if (text.empty()) {
            text = "Modification";
        }
    }
    if (entry.operations > 1) {
        text += " · " + std::to_string(entry.operations) + " opérations";
    }
    if (motifs.empty()) {
        return text + "\nMotif non précisé";
    }
    for (const auto& span : motifs) {
        const EditMotif& motif = span.motif;
        const char* author = motif.author == MotifAuthor::Person ? "Personne"
                             : motif.author == MotifAuthor::Procedure
                                 ? "Procédure"
                                 : "Agent";
        text += "\n" + std::string(author) + " · " + ReasonTitle(motif.reason);
        if (!motif.note.empty()) {
            text += " — " + motif.note;
        }
    }
    return text;
}

// Read the applied tail directly. A bounded summary of the whole history
// can contain only undone entries after a deep undo, hiding every change
// that is still in effect.
inline std::vector<std::string> RecentChanges(const EditLog& log,
                                              size_t limit = 8) {
    std::vector<std::string> lines;
    const auto& entries = log.AppliedEntries();
    for (auto entry = entries.rbegin();
         entry != entries.rend() && lines.size() < limit; ++entry) {
        HistoryEntry summary;
        summary.type = OperationTypeName(entry->op);
        summary.operations = EntryMemberCount(entry->op);
        if (const auto* batch = std::get_if<BatchOperation>(&entry->op)) {
            summary.label = batch->label;
        }
        lines.push_back(ChangeText(summary, entry->motifs));
    }
    return lines;
}

}  // namespace ui::workspace
