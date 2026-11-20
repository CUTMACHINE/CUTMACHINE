#include "ChatPresentation.h"

#include <iostream>
#include <string>

#include "TestSupport.h"

namespace {}  // namespace

int main() {
    const std::string report = "{\"compte_rendu\":1,\"propos\":\"Un film.\"}\n";
    const auto shown = chat::PresentChatMessage(
        "**Livré** : [film.mp4](/tmp/film.mp4).\n\n"
        "À revoir : un mot à 8,5 s.\n\n```json\n" +
        report + "```\n\nUne réserve après le rapport.");
    Check(shown.markdown.find("À revoir : un mot à 8,5 s.") !=
                  std::string::npos &&
              shown.markdown.find("Une réserve après le rapport.") !=
                  std::string::npos,
          "warnings before and after the report stay visible");
    Check(shown.markdown.find("compte_rendu") == std::string::npos &&
              shown.technical_report == report,
          "the complete report moves without rewriting its bytes");
    Check(shown.markdown.find("[film.mp4](/tmp/film.mp4)") != std::string::npos,
          "the delivery link stays in the human message");

    for (const std::string text :
         {"```json\n{\"warning\":\"Un mot manque\"}\n```",
          "```json\n{\"compte_rendu\":1, broken}\n```",
          "```json\n{\"compte_rendu\":1}\n",
          "```json\n{\"compte_rendu\":1}\n    ```",
          "```json\n{\"compte_rendu\":2}\n```",
          "```json\n{\"compte_rendu\":\"1\"}\n```",
          "````text\n```json\n{\"compte_rendu\":1}\n```\n````",
          "Texte ordinaire, **gras** et `code`."}) {
        const auto ordinary = chat::PresentChatMessage(text);
        Check(ordinary.markdown == text && ordinary.technical_report.empty(),
              "ordinary, invalid, nested and unfinished code stays visible");
    }
    const auto state = chat::PresentChatMessage(
        "État conservé.\r\n\r\n~~~json\r\n"
        "{\"etat_montage\":1}\r\n~~~\r\n");
    Check(state.markdown == "État conservé." &&
              state.technical_report == "{\"etat_montage\":1}\r\n",
          "the phase state accepts tilde fences and Windows newlines");
    const std::string large =
        "{\"compte_rendu\":1,\"texte\":\"" + std::string(100000, 'a') + "\"}\n";
    const auto full =
        chat::PresentChatMessage("Fini.\n```json\n" + large + "```");
    Check(full.markdown == "Fini." && full.technical_report == large,
          "a long report is preserved in full, never truncated");
    return failures == 0 ? 0 : 1;
}
