#pragma once

// AX-2026-09 AX6 -- docs/roadmap/AX-2026-09.md. The font families a motion
// text or a caption can name, as the engine finds them. Agents listed
// ~/Library/Fonts to choose one, and that folder says neither what the
// engine finds nor what it does not.
//
// Motion is laid out and drawn by Core Text: the families installed on this
// Mac, plus the directories a call registers, and a family it lacks is
// refused (FontMissing). Captions were drawn into the file by FFmpeg's
// drawtext, which asked fontconfig -- blind to Adobe Fonts -- and took
// another family without a word. Since LG3 (docs/roadmap/LEGENDES-2026-10.md)
// they are drawn by Core Text too, resolving a family as motion does; a
// family it lacks is drawn in Helvetica Neue and said. So the listing names
// the families Core Text has, and among them the rare ones a caption would
// still not be drawn in (a family without a face the caption takes).
//
// Core Text sits behind a seam the executable installs, as the motion
// renderer does (MotionFrameRenderer.h); the rest is plain C++.

#include "Json.h"
#include "MotionComposition.h"

#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

// What caption drawing resolves a family with: the caption renderer
// (CaptionRenderer.h), Core Text.
struct CaptionFontFinder {
    // The renderer was asked, and `has` answers for it.
    bool checked = false;
    // Why it was not, when it was not.
    std::string reason;
    // Whether a caption in this family is drawn in it.
    std::function<bool(const std::string&)> has;
};

// The installed caption renderer, or unchecked with the reason when this
// process has none.
CaptionFontFinder ReadCaptionFontFinder();

struct FontCatalog {
    // The families Core Text knows, sorted without case, its hidden system
    // families (".SF NS"...) left out, narrowed by the query.
    std::vector<std::string> families;
    // Those of `families` a caption is not drawn in: a caption in one of
    // them exports in Helvetica Neue (a family of italic faces only).
    std::vector<std::string> not_in_captions;
    CaptionFontFinder captions;
};

// `query`: part of a family name, without case; empty keeps them all.
FontCatalog BuildFontCatalog(const std::vector<std::string>& motionFamilies,
                             CaptionFontFinder captions,
                             const std::string& query);

// The answer of list_fonts and --list-fonts, without their "ok".
mcp_json::Value FontCatalogJson(const FontCatalog& catalog);

struct Document;

// The families the cartons of `document`'s visible caption tracks ask for
// (CaptionFontName: "system" is Helvetica Neue), each once, in the order
// they first appear.
std::vector<std::string> CaptionFontFamilies(const Document& document);

// What check_caption_legibility says of those families: the ones the
// caption renderer does not have, whose cartons were drawn in Helvetica
// Neue; or, unchecked, why that could not be told.
mcp_json::Value CaptionFontsJson(const std::vector<std::string>& families,
                                 const CaptionFontFinder& finder);

// Decided by the author on 8 October 2026 (ROADMAP.md, « À trancher »): a caption
// whose family is missing is exported all the same, in another family, and
// the export says so -- in its answer, and in a warning the window shows
// before it starts. Since LG3 the other family is always Helvetica Neue
// (kCaptionFallbackFamily), where fontconfig used to pick one.
struct CaptionFontSubstitution {
    std::string family;
    // The family the cartons are drawn in instead.
    std::string exported_as;

    bool operator==(const CaptionFontSubstitution&) const = default;
};

// The families of `families` the finder does not have, each once, in order,
// with the fallback the renderer draws them in. None when the finder could
// not check: then a caption export is refused anyway for want of a
// renderer, which check_caption_legibility says.
std::vector<CaptionFontSubstitution> FindCaptionFontSubstitutions(
    const std::vector<std::string>& families, const CaptionFontFinder& finder);

// The export's check, with the installed renderer. Asks nothing for no
// family.
std::vector<CaptionFontSubstitution> CheckCaptionFonts(
    const std::vector<std::string>& families);

// [{"family", "exported_as"}].
mcp_json::Value CaptionFontSubstitutionsJson(
    const std::vector<CaptionFontSubstitution>& substitutions);

// The window's warning before such an export, in French.
struct CaptionFontNotice {
    std::string title;
    std::string text;
};
CaptionFontNotice CaptionFontWarning(
    const std::vector<CaptionFontSubstitution>& substitutions);

// The Core Text families once `font_directories` are registered for the
// process (RegisterMotionFontDirectory). False with the error when a
// directory cannot be.
using MotionFontLister =
    bool (*)(const std::vector<std::string>& font_directories,
             std::vector<std::string>& families, MotionError& error);

namespace motion_font_lister {
inline std::atomic<MotionFontLister>& Slot() {
    static std::atomic<MotionFontLister> lister{nullptr};
    return lister;
}
}  // namespace motion_font_lister

inline void SetMotionFontLister(MotionFontLister lister) {
    motion_font_lister::Slot().store(lister);
}

// Null when this process has no lister (a test that links the model alone).
inline MotionFontLister CurrentMotionFontLister() {
    return motion_font_lister::Slot().load();
}

// The whole listing for a surface: Core Text through the seam, and the
// caption renderer. False with the error when no lister is installed or a
// directory cannot be registered.
bool ListFonts(const std::vector<std::string>& fontDirectories,
               const std::string& query, FontCatalog& catalog,
               MotionError& error);
