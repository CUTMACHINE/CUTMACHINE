// AX-2026-09 AX6 -- docs/roadmap/AX-2026-09.md. What FontCatalog.h decides
// without Core Text: which families are listed, and which a caption would
// export in another font. Portable: the Core Text side -- the font lister
// and, since LG3 (docs/roadmap/LEGENDES-2026-10.md), the caption renderer's
// family check -- is a seam this suite fills itself.

#include "FontCatalog.h"

#include "Document.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Joined(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        joined += (joined.empty() ? "" : "|") + name;
    }
    return joined;
}

// A caption renderer that has these families, compared without case as
// Core Text compares them.
CaptionFontFinder Knowing(const std::vector<std::string>& families) {
    CaptionFontFinder finder;
    finder.checked = true;
    finder.has = [families](const std::string& family) {
        const auto folded = [](std::string text) {
            for (char& character : text) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return text;
        };
        for (const std::string& known : families) {
            if (folded(known) == folded(family)) return true;
        }
        return false;
    };
    return finder;
}

std::vector<std::string> listed;

bool FakeLister(const std::vector<std::string>& directories,
                std::vector<std::string>& families, MotionError& error) {
    if (!directories.empty() && directories.front() == "/missing") {
        error = {"IoError", "fonts", "font directory does not exist"};
        return false;
    }
    families = listed;
    return true;
}

}  // namespace

int main() {
    // Core Text's families: sorted without case, hidden ones out, each once.
    const FontCatalog all = BuildFontCatalog(
        {"avenir", ".SF NS", "Zapfino", "Avenir Next", "Avenir", "Poppins", ""},
        Knowing({"Avenir", "Avenir Next", "Zapfino"}), "");
    Check(Joined(all.families) == "avenir|Avenir Next|Poppins|Zapfino",
          "families sorted without case, hidden and repeated left out: " +
              Joined(all.families));
    Check(Joined(all.not_in_captions) == "Poppins",
          "a family the caption renderer lacks is named for captions: " +
              Joined(all.not_in_captions));

    const FontCatalog queried = BuildFontCatalog(
        {"Avenir", "Avenir Next", "Zapfino"}, Knowing({}), "NEXT");
    Check(Joined(queried.families) == "Avenir Next",
          "a query keeps the families containing it, without case: " +
              Joined(queried.families));

    // Unchecked: nothing is said missing, and the reason is given.
    CaptionFontFinder unchecked;
    unchecked.reason = "no caption renderer";
    const FontCatalog blind = BuildFontCatalog({"Poppins"}, unchecked, "");
    const std::string blindJson = FontCatalogJson(blind).Dump();
    Check(blind.not_in_captions.empty() &&
              blindJson ==
                  "{\"count\":1,\"families\":[\"Poppins\"],\"captions\":{"
                  "\"checked\":false,\"reason\":\"no caption "
                  "renderer\"}}",
          "without a renderer, the reason and no verdict: " + blindJson);
    const std::string allJson = FontCatalogJson(all).Dump();
    Check(allJson.find("\"captions\":{\"checked\":true,\"missing\":"
                       "[\"Poppins\"]}") != std::string::npos,
          "checked, the families captions miss: " + allJson);

    // check_caption_legibility's fonts: the families the visible caption
    // tracks ask for, "system" as Helvetica Neue, each once; and those the
    // caption renderer does not have.
    Document document;
    CaptionStyle poppins;
    poppins.id = "01K90000000000000000000001";
    poppins.font_family = "Poppins";
    document.sequence.caption_styles.push_back(poppins);
    DocumentTrack captions;
    captions.kind = "caption";
    DocumentClip styled;
    styled.caption_group_id = poppins.id;
    DocumentClip plain;
    captions.clips = {styled, plain, styled};
    DocumentTrack hidden = captions;
    hidden.visible = false;
    CaptionStyle avenir = poppins;
    avenir.id = "01K90000000000000000000002";
    avenir.font_family = "Avenir Next";
    document.sequence.caption_styles.push_back(avenir);
    DocumentClip hiddenClip;
    hiddenClip.caption_group_id = avenir.id;
    hidden.clips = {hiddenClip};
    document.sequence.tracks = {captions, hidden};
    const std::vector<std::string> used = CaptionFontFamilies(document);
    Check(Joined(used) == "Poppins|Helvetica Neue",
          "the visible cartons' families, each once: " + Joined(used));
    const std::string fontsJson =
        CaptionFontsJson(used, Knowing({"Helvetica Neue"})).Dump();
    Check(fontsJson ==
              "{\"families\":[\"Poppins\",\"Helvetica Neue\"],"
              "\"checked\":true,\"missing\":[\"Poppins\"]}",
          "the family the caption renderer lacks: " + fontsJson);
    const std::string blindFonts = CaptionFontsJson(used, unchecked).Dump();
    Check(blindFonts.find("\"checked\":false,\"reason\":\"no caption") !=
              std::string::npos,
          "unchecked, the reason: " + blindFonts);

    // The seam: none installed is a refusal by name; a lister's own error
    // comes back as it is; its families reach the catalog.
    FontCatalog catalog;
    MotionError error;
    SetMotionFontLister(nullptr);
    Check(!ListFonts({}, "", catalog, error) &&
              error.code == "UnsupportedOperation",
          "no Core Text in the process: refused by name");
    SetMotionFontLister(&FakeLister);
    error = {};
    Check(
        !ListFonts({"/missing"}, "", catalog, error) && error.code == "IoError",
        "a directory that cannot be registered is the lister's refusal");
    listed = {"Zapfino", ".Hidden", "Avenir"};
    error = {};
    Check(ListFonts({}, "", catalog, error) &&
              Joined(catalog.families) == "Avenir|Zapfino" &&
              (catalog.captions.checked || !catalog.captions.reason.empty()),
          "the lister's families are listed, and captions checked or the "
          "reason given: " +
              Joined(catalog.families));
    SetMotionFontLister(nullptr);

    // Decided on 8 October 2026: a caption family that is missing is
    // exported in another, and said. Since LG3 the other is always
    // Helvetica Neue, the renderer's fallback. Each family once, without
    // case; nothing when unchecked.
    const std::vector<CaptionFontSubstitution> substituted =
        FindCaptionFontSubstitutions(
            {"Poppins", "Helvetica Neue", "poppins", "Roboto"},
            Knowing({"Helvetica Neue"}));
    Check(substituted ==
              std::vector<CaptionFontSubstitution>{
                  {"Poppins", "Helvetica Neue"}, {"Roboto", "Helvetica Neue"}},
          "the families the renderer lacks, once each, in Helvetica Neue: " +
              CaptionFontSubstitutionsJson(substituted).Dump());
    Check(
        FindCaptionFontSubstitutions({"Poppins"}, CaptionFontFinder{}).empty(),
        "renderer unchecked: nothing said");
    Check(CaptionFontSubstitutionsJson(substituted).Dump() ==
              R"([{"family":"Poppins","exported_as":"Helvetica Neue"},)"
              R"({"family":"Roboto","exported_as":"Helvetica Neue"}])",
          "the answer's list: " +
              CaptionFontSubstitutionsJson(substituted).Dump());
    Check(CheckCaptionFonts({}).empty(), "no caption family: nothing asked");

    // The window's warning names each family and what replaces it.
    const CaptionFontNotice one =
        CaptionFontWarning({{"Poppins", "Helvetica Neue"}});
    Check(one.title == "Une police de sous-titres manque à l’export" &&
              one.text.rfind("« Poppins » sortira en « Helvetica Neue ».\n",
                             0) == 0 &&
              one.text.find("Adobe Fonts") != std::string::npos,
          "one family: " + one.title + " / " + one.text);
    const CaptionFontNotice two = CaptionFontWarning(
        {{"Poppins", "Helvetica Neue"}, {"Roboto", "Helvetica Neue"}});
    Check(two.title == "Des polices de sous-titres manquent à l’export" &&
              two.text.find("« Roboto » sortira en « Helvetica Neue ».") !=
                  std::string::npos,
          "two families: " + two.text);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All font catalog tests passed\n";
    return 0;
}
