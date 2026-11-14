#pragma once

// ON8 -- docs/roadmap/ONGLETS-2026-10.md. The trim mode, made visible. The author,
// 31 October 2026, on what FC4 wired: « bcp de trucs invisibles ». Nothing
// said the window was in trim mode, which side of the cut was taken, nor
// which keys acted: V, U, comma and period, E and W were known or not at
// all. The bar beside the Trim tool says it and offers it: the side (A,
// Roll, B), one and five frames each way, Extend, the dynamic trim and the
// way out, each button carrying its key. Every button does what its key or
// its entry of the Trim menu does; nothing here trims.
//
// Policy only, plain C++: the window shows what DescribeTrimBar says and
// sends back the action of the button pressed.

#include "TrimSession.h"

#include <functional>
#include <string>
#include <vector>

namespace trim {

struct TrimBarButton {
    // What main.mm does when it is pressed: "select", "side.a",
    // "side.roll", "side.b", "nudge.-5", "nudge.-1", "nudge.+1",
    // "nudge.+5", "extend", "dynamic", "play.around", "leave".
    std::string action;
    std::string title;
    // Its tooltip: what it does, and its key.
    std::string help;
    bool enabled = true;
    // A side taken, the dynamic trim on.
    bool on = false;
};

struct TrimBarModel {
    // Shown while the Trim tool is the tool or a point is selected.
    bool shown = false;
    // "TRIM · ripple", "TRIM · simple", with the number of points.
    std::string label;
    std::vector<TrimBarButton> buttons;
};

// `keyFor` gives the key bound to a command of KeyboardShortcuts.h as the
// menus write it ("V", "⇧."), empty when it has none.
inline TrimBarModel DescribeTrimBar(
    const TrimSession& session, const Document& document, bool trimTool,
    bool dynamicMode,
    const std::function<std::string(const std::string&)>& keyFor) {
    TrimBarModel model;
    model.shown = trimTool || session.Active();
    if (!model.shown) return model;
    const auto withKey = [&](const std::string& what,
                             const std::string& command) {
        const std::string key = keyFor ? keyFor(command) : std::string();
        return key.empty() ? what : what + " · " + key;
    };
    if (!session.Active()) {
        model.label = "TRIM";
        model.buttons.push_back(
            {"select", "Prendre le raccord",
             withKey("Le point de montage le plus proche de la tête de lecture",
                     "trim.select"),
             true, false});
        return model;
    }
    const std::vector<TrimPoint>& points = session.Points();
    model.label = session.Style() == TrimStyle::Ripple ? "TRIM · ripple"
                                                       : "TRIM · simple";
    if (points.size() > 1)
        model.label += " · " + std::to_string(points.size()) + " points";
    const bool still = !session.Dynamic().active;
    const auto side = [&](TrimSide wanted, const char* action,
                          const char* title, const char* what) {
        bool all = true;
        for (const TrimPoint& point : points) all = all && point.side == wanted;
        model.buttons.push_back(
            {action, title,
             withKey(std::string(what) + " ; la touche fait défiler les côtés",
                     "trim.side"),
             still && session.CanSetSide(document, wanted), all});
    };
    side(TrimSide::A, "side.a", "A", "Sortie : la fin du plan d'avant");
    side(TrimSide::Roll, "side.roll", "Roll",
         "Roll : la coupe se déplace, la durée reste");
    side(TrimSide::B, "side.b", "B", "Entrée : le début du plan d'après");
    const auto nudge = [&](const char* action, const char* title,
                           const char* what, const char* command) {
        model.buttons.push_back(
            {action, title, withKey(what, command), still, false});
    };
    nudge("nudge.-5", "−5", "Cinq images en arrière", "trim.nudge.back.many");
    nudge("nudge.-1", "−1", "Une image en arrière", "trim.nudge.back");
    nudge("nudge.+1", "+1", "Une image en avant", "trim.nudge.forward");
    nudge("nudge.+5", "+5", "Cinq images en avant", "trim.nudge.forward.many");
    model.buttons.push_back(
        {"extend", "Étendre",
         withKey("Le point à la tête de lecture", "trim.extend"), still,
         false});
    model.buttons.push_back(
        {"dynamic", "Dynamique",
         withKey("Trim dynamique : J et L trimment, K écrit, Échap abandonne",
                 "trim.dynamic"),
         true, dynamicMode});
    // ON9 -- the cut played from before to after it, judged again after
    // each nudge; Command-slash loops it.
    model.buttons.push_back(
        {"play.around", "Lire autour",
         withKey("Jouer de 2 s avant à 2 s après le raccord, puis y revenir",
                 "play.around"),
         still, false});
    model.buttons.push_back(
        {"leave", "Quitter", "Quitter le mode trim · Échap", true, false});
    return model;
}

}  // namespace trim
