#pragma once

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The form panel of the window
// (FormPanel.h) decided here: where its title, its explanation, the
// caller's form, its error line and its buttons go, which button Return and
// Escape press, what each button does to the panel, and the colours it
// takes. Plain C++, no AppKit: FormPanel.mm measures the texts and the
// buttons, asks this file for the frames and applies them
// (tests/form_panel_layout_tests.cc).
//
// The forms of the window used to live in the accessory view of an NSAlert:
// a warning icon, a centred title, the whole application blocked, no
// resizing, and a refusal that came once the alert had closed, in a second
// alert. The panel is a sheet on the window instead; it stays open on a
// refusal and says it on its error line, under the form.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "UiTheme.h"

namespace ui::form_panel {

// What a button does to the panel.
enum class ButtonRole : std::uint8_t {
    // Return. The caller's check runs first: the panel closes only when it
    // has nothing to say.
    kAccept,
    // Escape. Closes, nothing checked.
    kCancel,
    // Closes with its own answer, after the check (« Supprimer le
    // marqueur »).
    kAlternate,
    // The caller's own action: the panel stays open (« Copier le
    // diagnostic », « Valeurs par défaut »).
    kAction,
};

constexpr bool Closes(ButtonRole role) { return role != ButtonRole::kAction; }

constexpr bool Validates(ButtonRole role) {
    return role == ButtonRole::kAccept || role == ButtonRole::kAlternate;
}

// The key a button answers: Return for the one that accepts, Escape for the
// one that cancels, none for the others.
constexpr const char* KeyEquivalent(ButtonRole role) {
    return role == ButtonRole::kAccept   ? "\r"
           : role == ButtonRole::kCancel ? "\x1b"
                                         : "";
}

// The accepting and the cancelling buttons sit at the right, the accepting
// one rightmost; the others at the left, apart from them, so that neither a
// destructive answer nor an action that keeps the panel open sits where the
// hand expects « OK ».
constexpr bool Trailing(ButtonRole role) {
    return role == ButtonRole::kAccept || role == ButtonRole::kCancel;
}

// Escape on a panel without a cancelling button: an information panel whose
// one closing button is « Fermer » closes, a form that only accepts does
// not. The index of the button Escape presses, or -1.
inline int EscapeButton(const std::vector<ButtonRole>& roles) {
    int closing = -1;
    int closingCount = 0;
    for (size_t index = 0; index < roles.size(); ++index) {
        if (roles[index] == ButtonRole::kCancel) {
            return static_cast<int>(index);
        }
        if (Closes(roles[index])) {
            closing = static_cast<int>(index);
            ++closingCount;
        }
    }
    return closingCount == 1 ? closing : -1;
}

// ---- Colours and type ----
// A raised surface over the window's panels, the window's own text colours.
// The error line is the primary text after an icon in the error colour: the
// error colour itself is too dark on this surface for twelve-point text
// (3.1:1), as on the welcome screen's notice.
inline constexpr ui::theme::Color kSurface = ui::theme::kSurfaceRaised;
inline constexpr ui::theme::Color kTitleText = ui::theme::kTextPrimary;
inline constexpr ui::theme::Color kMessageText = ui::theme::kTextSecondary;
inline constexpr ui::theme::Color kErrorText = ui::theme::kTextPrimary;
inline constexpr ui::theme::Color kErrorIcon = ui::theme::kError;
inline constexpr double kTitleFontSize = ui::theme::kFontSizeTitle;
inline constexpr double kBodyFontSize = ui::theme::kFontSizeBody;
inline constexpr double kErrorIconPointSize = ui::theme::kFontSizeSection;

// ---- Metrics (points) ----
inline constexpr double kMargin = ui::theme::kSpaceXl;
inline constexpr double kTitleToMessage = ui::theme::kSpaceXs;
inline constexpr double kTextToContent = ui::theme::kSpaceL;
inline constexpr double kContentToError = ui::theme::kSpaceS;
inline constexpr double kErrorToButtons = ui::theme::kSpaceM;
inline constexpr double kButtonGap = ui::theme::kSpaceM;
inline constexpr double kButtonGroupGap = ui::theme::kSpaceXl;
inline constexpr double kErrorIconSize = ui::theme::kSpaceL;
inline constexpr double kErrorIconGap = ui::theme::kSpaceS;
// A push button is never narrower than this: « OK » and « Créer » get the
// width of « Annuler ».
inline constexpr double kButtonMinWidth = 5.0 * ui::theme::kSpaceL;
// Nor the panel: a one-field form still reads as a dialog, its explanation
// on one or two lines rather than five.
inline constexpr double kMinWidth = 420.0;

// AppKit's frame convention: the origin at the bottom left, y upwards.
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Size {
    double width = 0.0;
    double height = 0.0;
};

struct Button {
    ButtonRole role = ButtonRole::kAccept;
    // As AppKit fits its title; kButtonMinWidth is applied here.
    double width = 0.0;
};

struct Input {
    // The caller's form, at the size it gives it.
    Size content;
    // One line each, measured.
    double title_width = 0.0;
    double title_height = 0.0;
    // Measured at TextWidth(); 0 without an explanation.
    double message_height = 0.0;
    // One line of the error's font: the error line keeps it even when it is
    // empty, so that a refusal does not make the panel jump.
    double error_line_height = 0.0;
    // The error, measured at ErrorTextWidth(); 0 without one.
    double error_height = 0.0;
    double button_height = 0.0;
    std::vector<Button> buttons;
    // A resizable panel: the form takes the panel's whole width.
    bool stretch_content = false;
    // The width the panel has already been given -- by the person, who
    // resized it -- or 0 for its natural width. Never narrower than that.
    double width = 0.0;
};

struct Frames {
    // The panel's content size.
    Size panel;
    Rect title;
    // Height 0 without an explanation.
    Rect message;
    Rect content;
    Rect error_icon;
    Rect error_text;
    // In the order of Input::buttons.
    std::vector<Rect> buttons;
};

inline double ButtonWidth(const Button& button) {
    return std::max(button.width, kButtonMinWidth);
}

// The width the buttons take on their row, both groups and the gap between
// them included.
inline double ButtonRowWidth(const std::vector<Button>& buttons) {
    double trailing = 0.0;
    double leading = 0.0;
    size_t trailingCount = 0;
    size_t leadingCount = 0;
    for (const Button& button : buttons) {
        if (Trailing(button.role)) {
            trailing += ButtonWidth(button);
            ++trailingCount;
        } else {
            leading += ButtonWidth(button);
            ++leadingCount;
        }
    }
    if (trailingCount > 1) {
        trailing += kButtonGap * static_cast<double>(trailingCount - 1);
    }
    if (leadingCount > 1) {
        leading += kButtonGap * static_cast<double>(leadingCount - 1);
    }
    if (trailingCount > 0 && leadingCount > 0) {
        return trailing + kButtonGroupGap + leading;
    }
    return trailing + leading;
}

// The panel's width: what its form, its title and its buttons need, never
// under kMinWidth, nor under what it has already been given.
inline double PanelWidth(const Input& input) {
    const double natural =
        std::max({kMinWidth, input.content.width + 2.0 * kMargin,
                  input.title_width + 2.0 * kMargin,
                  ButtonRowWidth(input.buttons) + 2.0 * kMargin});
    return std::max(natural, input.width);
}

// The width the title and the explanation wrap at, and the error's, after
// its icon.
inline double TextWidth(double panelWidth) {
    return panelWidth - 2.0 * kMargin;
}

inline double ErrorTextWidth(double panelWidth) {
    return TextWidth(panelWidth) - kErrorIconSize - kErrorIconGap;
}

inline Frames Compute(const Input& input) {
    Frames frames;
    const double width = PanelWidth(input);
    const double text = TextWidth(width);

    // From the bottom: the buttons, the error line, the form, the
    // explanation, the title.
    double y = kMargin;
    frames.buttons.resize(input.buttons.size());
    double trailingX = width - kMargin;
    double leadingX = kMargin;
    for (size_t index = 0; index < input.buttons.size(); ++index) {
        const Button& button = input.buttons[index];
        const double buttonWidth = ButtonWidth(button);
        double x = 0.0;
        if (Trailing(button.role)) {
            trailingX -= buttonWidth;
            x = trailingX;
            trailingX -= kButtonGap;
        } else {
            x = leadingX;
            leadingX += buttonWidth + kButtonGap;
        }
        frames.buttons[index] = {x, y, buttonWidth, input.button_height};
    }
    if (!input.buttons.empty()) {
        y += input.button_height + kErrorToButtons;
    }

    const double errorHeight =
        std::max(input.error_line_height, input.error_height);
    frames.error_text = {kMargin + kErrorIconSize + kErrorIconGap, y,
                         ErrorTextWidth(width), errorHeight};
    // The icon beside the error's first line.
    const double iconY = y + errorHeight - input.error_line_height +
                         (input.error_line_height - kErrorIconSize) / 2.0;
    frames.error_icon = {kMargin, iconY, kErrorIconSize, kErrorIconSize};
    y += errorHeight + kContentToError;

    const double contentWidth =
        input.stretch_content ? text : input.content.width;
    frames.content = {kMargin, y, contentWidth, input.content.height};
    y += input.content.height + kTextToContent;

    if (input.message_height > 0.0) {
        frames.message = {kMargin, y, text, input.message_height};
        y += input.message_height + kTitleToMessage;
    } else {
        frames.message = {kMargin, y, text, 0.0};
    }
    frames.title = {kMargin, y, text, input.title_height};
    y += input.title_height + kMargin;

    frames.panel = {width, y};
    return frames;
}

}  // namespace ui::form_panel
