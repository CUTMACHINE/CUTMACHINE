// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The form panel of the window
// (FormPanelLayout.h): the keys its buttons answer, what each button does to
// the panel, where everything goes, how a refusal grows it, and whether its
// texts read on its surface.
//
// Plain C++: no AppKit, no window.

#include "FormPanelLayout.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace panel = ui::form_panel;
using panel::ButtonRole;
using panel::Frames;
using panel::Input;
using panel::Rect;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

bool Near(double left, double right) { return std::abs(left - right) < 1e-9; }

double Top(const Rect& rect) { return rect.y + rect.height; }
double Right(const Rect& rect) { return rect.x + rect.width; }

bool Inside(const Rect& rect, const panel::Size& size) {
    return rect.x >= 0.0 && rect.y >= 0.0 && Right(rect) <= size.width &&
           Top(rect) <= size.height;
}

bool Overlap(const Rect& left, const Rect& right) {
    if (left.width <= 0.0 || left.height <= 0.0 || right.width <= 0.0 ||
        right.height <= 0.0) {
        return false;
    }
    return left.x < Right(right) && right.x < Right(left) &&
           left.y < Top(right) && right.y < Top(left);
}

// WCAG's contrast on sRGB values: the panel is AppKit's, not a Metal layer.
double Luminance(const ui::theme::Color& color) {
    const auto linear = [](float channel) {
        return channel <= 0.04045f ? channel / 12.92
                                   : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) +
           0.0722 * linear(color.b);
}

double Contrast(const ui::theme::Color& left, const ui::theme::Color& right) {
    const double lighter = std::max(Luminance(left), Luminance(right));
    const double darker = std::min(Luminance(left), Luminance(right));
    return (lighter + 0.05) / (darker + 0.05);
}

// « Nouvelle timeline » as FormPanel.mm measures it: a 320-point name field,
// a title, a one-line explanation, « Créer » and « Annuler ».
Input NewTimeline() {
    Input input;
    input.content = {320.0, 24.0};
    input.title_width = 130.0;
    input.title_height = 19.0;
    input.message_height = 15.0;
    input.error_line_height = 15.0;
    input.button_height = 32.0;
    input.buttons = {{ButtonRole::kAccept, 62.0}, {ButtonRole::kCancel, 76.0}};
    return input;
}

}  // namespace

int main() {
    Test("Return accepts, Escape cancels, the others answer no key", [] {
        Check(std::string(panel::KeyEquivalent(ButtonRole::kAccept)) == "\r",
              "the accepting button answers Return");
        Check(std::string(panel::KeyEquivalent(ButtonRole::kCancel)) == "\x1b",
              "the cancelling one answers Escape");
        Check(
            std::string(panel::KeyEquivalent(ButtonRole::kAlternate)).empty() &&
                std::string(panel::KeyEquivalent(ButtonRole::kAction)).empty(),
            "a destructive answer and an action answer no key");
    });

    Test("only the accepting and the alternate answers are checked", [] {
        Check(panel::Validates(ButtonRole::kAccept) &&
                  panel::Validates(ButtonRole::kAlternate),
              "accept and alternate run the caller's check");
        Check(!panel::Validates(ButtonRole::kCancel) &&
                  !panel::Validates(ButtonRole::kAction),
              "cancel and an action do not");
        Check(panel::Closes(ButtonRole::kAccept) &&
                  panel::Closes(ButtonRole::kCancel) &&
                  panel::Closes(ButtonRole::kAlternate) &&
                  !panel::Closes(ButtonRole::kAction),
              "an action keeps the panel open, every other button closes it");
    });

    Test(
        "Escape presses the cancelling button, or an information "
        "panel's one closing button",
        [] {
            Check(panel::EscapeButton({ButtonRole::kAccept, ButtonRole::kCancel,
                                       ButtonRole::kAlternate}) == 1,
                  "the cancelling button wherever it is");
            Check(panel::EscapeButton(
                      {ButtonRole::kAccept, ButtonRole::kAction}) == 0,
                  "« Fermer » beside « Copier le diagnostic »");
            Check(panel::EscapeButton(
                      {ButtonRole::kAccept, ButtonRole::kAlternate}) == -1,
                  "two ways to close and no cancel: Escape guesses neither");
            Check(panel::EscapeButton({}) == -1, "no button, no key");
        });

    Test(
        "a one-field form: everything inside, nothing overlapping, in "
        "reading order",
        [] {
            const Frames frames = panel::Compute(NewTimeline());
            Check(Near(frames.panel.width, panel::kMinWidth),
                  "a narrow form gets the panel's minimum width");
            const std::vector<Rect> parts = {
                frames.title,      frames.message,    frames.content,
                frames.error_icon, frames.error_text, frames.buttons[0],
                frames.buttons[1]};
            bool inside = true;
            for (const Rect& part : parts) {
                inside = inside && Inside(part, frames.panel);
            }
            Check(inside, "every part lies inside the panel");
            bool apart = true;
            for (size_t left = 0; left < parts.size(); ++left) {
                for (size_t right = left + 1; right < parts.size(); ++right) {
                    apart = apart && !Overlap(parts[left], parts[right]);
                }
            }
            Check(apart, "no two parts overlap");
            Check(frames.title.y > Top(frames.message) &&
                      frames.message.y > Top(frames.content) &&
                      frames.content.y > Top(frames.error_text) &&
                      frames.error_text.y > Top(frames.buttons[0]),
                  "title, explanation, form, error line, buttons, from the "
                  "top");
            Check(
                Near(frames.panel.height - Top(frames.title), panel::kMargin) &&
                    Near(frames.buttons[0].y, panel::kMargin) &&
                    Near(frames.title.x, panel::kMargin) &&
                    Near(frames.content.x, panel::kMargin),
                "the margins come from the theme's spacing");
            Check(Near(frames.content.width, 320.0) &&
                      Near(frames.content.height, 24.0),
                  "the form keeps the size its caller gave it");
        });

    Test(
        "the accepting button is rightmost, the cancelling one beside it, "
        "the others at the left",
        [] {
            Input input = NewTimeline();
            input.buttons = {{ButtonRole::kAccept, 90.0},
                             {ButtonRole::kCancel, 76.0},
                             {ButtonRole::kAlternate, 150.0}};
            const Frames frames = panel::Compute(input);
            const Rect& accept = frames.buttons[0];
            const Rect& cancel = frames.buttons[1];
            const Rect& other = frames.buttons[2];
            Check(Near(Right(accept), frames.panel.width - panel::kMargin),
                  "the accepting button ends at the right margin");
            Check(Near(Right(cancel) + panel::kButtonGap, accept.x),
                  "the cancelling one sits left of it, a gap apart");
            Check(Near(other.x, panel::kMargin) &&
                      other.x + other.width + panel::kButtonGroupGap <=
                          cancel.x + 1e-9,
                  "the destructive answer sits at the left margin, well "
                  "apart");
            Check(Near(accept.y, cancel.y) && Near(cancel.y, other.y),
                  "on one row");
        });

    Test("a button is never narrower than « Annuler »", [] {
        Input input = NewTimeline();
        input.buttons = {{ButtonRole::kAccept, 30.0}};
        const Frames frames = panel::Compute(input);
        Check(Near(frames.buttons[0].width, panel::kButtonMinWidth),
              "« OK » gets the minimum width");
    });

    Test("the panel widens for what it holds", [] {
        Input input = NewTimeline();
        input.content = {650.0, 565.0};
        Check(Near(panel::Compute(input).panel.width,
                   650.0 + 2.0 * panel::kMargin),
              "for a wide form");
        input = NewTimeline();
        input.title_width = 500.0;
        Check(Near(panel::Compute(input).panel.width,
                   500.0 + 2.0 * panel::kMargin),
              "for a long title");
        input = NewTimeline();
        input.buttons = {{ButtonRole::kAccept, 200.0},
                         {ButtonRole::kCancel, 120.0},
                         {ButtonRole::kAction, 180.0}};
        const Frames frames = panel::Compute(input);
        Check(Near(frames.panel.width, 200.0 + panel::kButtonGap + 120.0 +
                                           panel::kButtonGroupGap + 180.0 +
                                           2.0 * panel::kMargin),
              "for a long row of buttons");
        Check(!Overlap(frames.buttons[1], frames.buttons[2]),
              "whose groups do not meet");
    });

    Test(
        "a panel resized by the person keeps its width, its form "
        "stretched",
        [] {
            Input input = NewTimeline();
            input.stretch_content = true;
            input.width = 700.0;
            const Frames frames = panel::Compute(input);
            Check(Near(frames.panel.width, 700.0), "the width it was given");
            Check(Near(frames.content.width, panel::TextWidth(700.0)),
                  "the form takes the panel's width");
            input.width = 100.0;
            Check(Near(panel::Compute(input).panel.width, panel::kMinWidth),
                  "never narrower than what the panel needs");
        });

    Test("a refusal grows the error line, and the panel by as much", [] {
        Input input = NewTimeline();
        const Frames empty = panel::Compute(input);
        Check(Near(empty.error_text.height, input.error_line_height),
              "an empty error line keeps one line");
        input.error_height = 15.0;
        const Frames one = panel::Compute(input);
        Check(Near(one.panel.height, empty.panel.height),
              "a one-line refusal does not make the panel jump");
        input.error_height = 45.0;
        const Frames three = panel::Compute(input);
        Check(Near(three.panel.height - empty.panel.height, 30.0),
              "three lines grow the panel by two lines");
        Check(Near(three.content.height, empty.content.height) &&
                  Near(three.panel.height - Top(three.content),
                       empty.panel.height - Top(empty.content)) &&
                  Near(three.panel.height - Top(three.title),
                       empty.panel.height - Top(empty.title)),
              "the form, the explanation and the title keep their place "
              "from the top");
        Check(Near(Top(three.error_icon) - Top(three.error_text),
                   Top(one.error_icon) - Top(one.error_text)),
              "the icon stays beside the first line");
        Check(Near(three.error_text.width,
                   panel::ErrorTextWidth(three.panel.width)) &&
                  Near(three.error_text.x, three.error_icon.x +
                                               panel::kErrorIconSize +
                                               panel::kErrorIconGap),
              "the error wraps after its icon");
    });

    Test("without an explanation the form comes under the title", [] {
        Input input = NewTimeline();
        input.message_height = 0.0;
        const Frames frames = panel::Compute(input);
        Check(Near(frames.message.height, 0.0) &&
                  Near(frames.title.y - Top(frames.content),
                       panel::kTextToContent),
              "the title's gap to the form, nothing between");
    });

    Test("a progress panel without a button keeps its margins", [] {
        Input input = NewTimeline();
        input.buttons.clear();
        const Frames frames = panel::Compute(input);
        Check(Near(frames.error_text.y, panel::kMargin),
              "the error line sits on the bottom margin");
    });

    Test("the panel's texts read on its surface", [] {
        Check(Contrast(panel::kTitleText, panel::kSurface) >= 7.0,
              "the title, 7:1");
        Check(Contrast(panel::kMessageText, panel::kSurface) >= 4.5,
              "the explanation, 4.5:1");
        Check(Contrast(panel::kErrorText, panel::kSurface) >= 4.5,
              "the error, 4.5:1");
        Check(Contrast(panel::kErrorIcon, panel::kSurface) >= 3.0,
              "its icon, 3:1 as a graphic");
        Check(Contrast(ui::theme::kError, panel::kSurface) < 4.5,
              "the error colour alone would not read as text, which is why "
              "the text is primary");
    });

    if (failures == 0) {
        std::cout << "form panel layout tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
