#include "FontCatalog.h"

#include "CaptionGeneration.h"
#include "CaptionRenderer.h"
#include "Document.h"

#include <algorithm>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

char FoldAscii(char character) {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

std::string Folded(const std::string& text) {
    std::string folded = text;
    std::transform(folded.begin(), folded.end(), folded.begin(), FoldAscii);
    return folded;
}

// Families in reading order: without case, then as written.
struct FamilyOrder {
    bool operator()(const std::string& left, const std::string& right) const {
        const auto less = [](char a, char b) {
            return FoldAscii(a) < FoldAscii(b);
        };
        if (std::lexicographical_compare(left.begin(), left.end(),
                                         right.begin(), right.end(), less)) {
            return true;
        }
        return !std::lexicographical_compare(right.begin(), right.end(),
                                             left.begin(), left.end(), less) &&
               left < right;
    }
};

Value Strings(const std::vector<std::string>& texts) {
    Value list = Value::MakeArray();
    for (const std::string& text : texts) {
        list.Push(Value::MakeString(text));
    }
    return list;
}

}  // namespace

CaptionFontFinder ReadCaptionFontFinder() {
    CaptionFontFinder finder;
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr || renderer->has_family == nullptr) {
        finder.reason = kNoCaptionRenderer;
        return finder;
    }
    finder.checked = true;
    finder.has = renderer->has_family;
    return finder;
}

FontCatalog BuildFontCatalog(const std::vector<std::string>& motionFamilies,
                             CaptionFontFinder captions,
                             const std::string& query) {
    FontCatalog catalog;
    const std::string wanted = Folded(query);
    std::set<std::string> seen;
    std::set<std::string, FamilyOrder> ordered;
    for (const std::string& family : motionFamilies) {
        // Core Text compares a family without case (MotionRender.mm): two
        // spellings are one family to a composition.
        if (family.empty() || family.front() == '.' ||
            !seen.insert(Folded(family)).second ||
            Folded(family).find(wanted) == std::string::npos) {
            continue;
        }
        ordered.insert(family);
    }
    catalog.families.assign(ordered.begin(), ordered.end());
    if (captions.checked) {
        for (const std::string& family : catalog.families) {
            if (!captions.has(family)) {
                catalog.not_in_captions.push_back(family);
            }
        }
    }
    catalog.captions = std::move(captions);
    return catalog;
}

Value FontCatalogJson(const FontCatalog& catalog) {
    Value result = Value::MakeObject();
    result.Set("count",
               Value::MakeInt(static_cast<int64_t>(catalog.families.size())));
    result.Set("families", Strings(catalog.families));
    Value captions = Value::MakeObject();
    captions.Set("checked", Value::MakeBool(catalog.captions.checked));
    if (catalog.captions.checked) {
        captions.Set("missing", Strings(catalog.not_in_captions));
    } else {
        captions.Set("reason", Value::MakeString(catalog.captions.reason));
    }
    result.Set("captions", std::move(captions));
    return result;
}

std::vector<std::string> CaptionFontFamilies(const Document& document) {
    std::vector<std::string> families;
    const CaptionStyle unstyled;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "caption" || !track.visible) {
            continue;
        }
        for (const DocumentClip& clip : track.clips) {
            const CaptionStyle* style =
                document.FindCaptionStyle(clip.caption_group_id);
            const std::string family =
                CaptionFontName(style != nullptr ? *style : unstyled);
            if (std::find(families.begin(), families.end(), family) ==
                families.end()) {
                families.push_back(family);
            }
        }
    }
    return families;
}

Value CaptionFontsJson(const std::vector<std::string>& families,
                       const CaptionFontFinder& finder) {
    Value fonts = Value::MakeObject();
    fonts.Set("families", Strings(families));
    fonts.Set("checked", Value::MakeBool(finder.checked));
    if (!finder.checked) {
        fonts.Set("reason", Value::MakeString(finder.reason));
        return fonts;
    }
    std::vector<std::string> missing;
    for (const std::string& family : families) {
        if (!finder.has(family)) {
            missing.push_back(family);
        }
    }
    fonts.Set("missing", Strings(missing));
    return fonts;
}

std::vector<CaptionFontSubstitution> FindCaptionFontSubstitutions(
    const std::vector<std::string>& families, const CaptionFontFinder& finder) {
    std::vector<CaptionFontSubstitution> substitutions;
    if (!finder.checked) {
        return substitutions;
    }
    // Core Text compares a family without case (MotionRender.mm): two
    // spellings are one family, said once.
    std::set<std::string> seen;
    for (const std::string& family : families) {
        if (!seen.insert(Folded(family)).second || finder.has(family)) {
            continue;
        }
        substitutions.push_back({family, kCaptionFallbackFamily});
    }
    return substitutions;
}

std::vector<CaptionFontSubstitution> CheckCaptionFonts(
    const std::vector<std::string>& families) {
    if (families.empty()) {
        return {};
    }
    return FindCaptionFontSubstitutions(families, ReadCaptionFontFinder());
}

Value CaptionFontSubstitutionsJson(
    const std::vector<CaptionFontSubstitution>& substitutions) {
    Value list = Value::MakeArray();
    for (const CaptionFontSubstitution& substitution : substitutions) {
        Value item = Value::MakeObject();
        item.Set("family", Value::MakeString(substitution.family));
        item.Set("exported_as",
                 substitution.exported_as.empty()
                     ? Value::MakeNull()
                     : Value::MakeString(substitution.exported_as));
        list.Push(std::move(item));
    }
    return list;
}

CaptionFontNotice CaptionFontWarning(
    const std::vector<CaptionFontSubstitution>& substitutions) {
    CaptionFontNotice notice;
    notice.title = substitutions.size() == 1
                       ? "Une police de sous-titres manque à l’export"
                       : "Des polices de sous-titres manquent à l’export";
    std::string lines;
    for (const CaptionFontSubstitution& substitution : substitutions) {
        lines += "« " + substitution.family + " » sortira " +
                 (substitution.exported_as.empty()
                      ? std::string("dans une autre police")
                      : "en « " + substitution.exported_as + " »") +
                 ".\n";
    }
    notice.text =
        lines +
        "\nCette police n’est ni installée sur ce Mac ni activée : Adobe "
        "Fonts n’active les siennes que tant que Creative Cloud tourne. "
        "Installez-la, ou ouvrez Creative Cloud, avant d’exporter.";
    return notice;
}

bool ListFonts(const std::vector<std::string>& fontDirectories,
               const std::string& query, FontCatalog& catalog,
               MotionError& error) {
    const MotionFontLister lister = CurrentMotionFontLister();
    if (lister == nullptr) {
        error = {"UnsupportedOperation", "",
                 "this process has no font lister (Core Text): use the "
                 "cutmachine executable"};
        return false;
    }
    std::vector<std::string> families;
    if (!lister(fontDirectories, families, error)) {
        return false;
    }
    catalog = BuildFontCatalog(families, ReadCaptionFontFinder(), query);
    return true;
}
