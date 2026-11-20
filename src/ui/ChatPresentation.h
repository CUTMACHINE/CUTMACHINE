#pragma once

// ACP8 -- docs/roadmap/ACP-2026-09.md. A delivery report has two readers:
// the person reviewing the film and the next agent reading its trace. Keep
// the machine report reachable without making it the conversation's body.
// This policy is portable; AppKit only renders the two parts.

#include <string>

namespace chat {

inline constexpr char kAgentReplyGuidance[] =
    "Dans le panneau, le bilan de livraison se lit en une centaine de mots : "
    "le livrable avec un lien cliquable, sa durée, puis les réserves ou "
    "choix qui demandent une relecture. Garde visibles les limites utiles ; "
    "ne dis pas que tout est conforme si une règle reste insatisfaite. "
    "N'énumère pas les outils, identifiants, chemins absolus ou mesures "
    "techniques dans ce bilan, sauf si la personne les demande. Développe "
    "si une réserve l'exige ou si la personne demande le détail. Si la "
    "procédure exige un compte rendu JSON, conserve-le dans un bloc json "
    "séparé après le bilan : le panneau le rend consultable à part.";

struct ChatMessagePresentation {
    std::string markdown;
    std::string technical_report;
};

// Only a complete, valid fenced JSON report with compte_rendu:1 or
// etat_montage:1 moves to technical_report. Ordinary code, malformed or
// unfinished reports, and every word outside the fence stay in markdown.
// Neither the transcript nor the model's original answer is changed.
ChatMessagePresentation PresentChatMessage(const std::string& text);

}  // namespace chat
