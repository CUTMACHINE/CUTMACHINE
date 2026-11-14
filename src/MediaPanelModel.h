#pragma once

// F2.3 -- ROADMAP.md: view-model logic for the Media panel's three tabs
// (Media / Audio / Captions). Plain C++, no AppKit -- everything here is a
// pure function of Document.h/Operations.h types, so it is directly
// unit-testable (see tests/media_panel_model_tests.cc) without a macOS
// toolchain, the same way UiTheme.h and PanelLayout.h are (see their
// respective tests).
//
// This header holds *only* things that are safe to compute without a
// document mutation: filtering/searching the existing library, describing
// existing caption styles, and building the exact Operations.h structs a
// caller then applies via EditLog::Apply. It never mutates a Document and
// never invents new document/operation state -- see ROADMAP.md F2.3's hard
// constraints and PHILOSOPHY.md principle 2. main.mm (AppKit, unverified
// here) is the only place that actually calls EditLog::Apply.

#include "Document.h"
#include "MediaTaskManager.h"
#include "Operations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ui::media_panel {

// The Media panel's three tabs, in display order. Unlike PanelLayout.h's
// PanelSlot (one slot per *dock*, switched via CMPanelHostView), these are
// sub-tabs *within* the single PanelSlot::Media slot -- see PanelLayout.h's
// file comment on why F2.3 does not need CMPanelHostView's dock-level tab
// chrome. A local NSView-based tab strip (CMTabStripView, UiComponents.h)
// switches between them; nothing here is ever serialized.
// EF1 -- docs/roadmap/EFFETS-2026-10.md. Effects: the library dragged onto
// the timeline (EffectsLibrary.h), beside the media it is laid on.
enum class Tab { Media, Audio, Captions, Effects };

inline const std::array<Tab, 4>& AllTabs() {
    static const std::array<Tab, 4> kTabs{Tab::Media, Tab::Audio, Tab::Captions,
                                          Tab::Effects};
    return kTabs;
}

// Matches the app's French UI (see PanelLayout.h's panel titles).
inline const char* TabTitle(Tab tab) {
    switch (tab) {
        case Tab::Media:
            return "Média";
        case Tab::Audio:
            return "Audio";
        case Tab::Captions:
            return "Légendes";
        case Tab::Effects:
            return "Effets";
    }
    return "?";
}

// The name the last shown tab is remembered by (UiPreferences.h), stable
// when the tabs are reordered or renamed.
inline const char* TabIdentifier(Tab tab) {
    switch (tab) {
        case Tab::Media:
            return "media";
        case Tab::Audio:
            return "audio";
        case Tab::Captions:
            return "captions";
        case Tab::Effects:
            return "effects";
    }
    return "media";
}

inline std::optional<Tab> TabFromIdentifier(const std::string& identifier) {
    for (Tab tab : AllTabs())
        if (identifier == TabIdentifier(tab)) return tab;
    return std::nullopt;
}

namespace detail {

inline std::string Lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool ContainsInsensitive(const std::string& haystack,
                                const std::string& needleLower) {
    if (needleLower.empty()) return true;
    return Lower(haystack).find(needleLower) != std::string::npos;
}

// Same bin-membership rule main.mm's -rebuildMediaList already applies:
// `binFilter` empty means "all bins"; a bin id of the literal string used by
// the caller for "no bin" (main.mm uses "__root__"/"__all__" sentinels at
// the AppKit layer) is translated by the caller into `wantRoot`/`Ulid{}`
// before reaching here, so this stays a plain data-model predicate with no
// AppKit-specific sentinel strings baked in.
inline bool MatchesBin(const Ulid& itemBinId, bool anyBin, bool wantRoot,
                       const Ulid& wantBinId) {
    if (anyBin) return true;
    if (wantRoot) return itemBinId.empty();
    return itemBinId == wantBinId;
}

}  // namespace detail

// Returns the folders displayed by a Finder-like bin browser at one level.
// The root is represented by an empty parent id; descendants never leak into
// the current directory and names are ordered deterministically.
inline std::vector<const DocumentBin*> DirectChildBins(
    const std::vector<DocumentBin>& bins, const Ulid& parentId,
    const std::string& search) {
    const std::string searchLower = detail::Lower(search);
    std::vector<const DocumentBin*> result;
    for (const DocumentBin& bin : bins) {
        if (bin.parent_id != parentId) continue;
        if (!searchLower.empty() &&
            !detail::ContainsInsensitive(bin.name, searchLower))
            continue;
        result.push_back(&bin);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    return result;
}

struct BinNavigationEntry {
    const DocumentBin* bin = nullptr;
    size_t depth = 0;
};

// Flattens the hierarchy in display order while preserving each folder's
// depth. The visited set keeps malformed cyclic input from trapping the UI.
inline std::vector<BinNavigationEntry> BinNavigationTree(
    const std::vector<DocumentBin>& bins) {
    std::vector<BinNavigationEntry> result;
    std::set<Ulid> visited;
    std::function<void(const Ulid&, size_t)> append = [&](const Ulid& parentId,
                                                          size_t depth) {
        for (const DocumentBin* bin : DirectChildBins(bins, parentId, "")) {
            if (!visited.insert(bin->id).second) continue;
            result.push_back({bin, depth});
            append(bin->id, depth + 1);
        }
    };
    append({}, 0);
    return result;
}

// ---- Audio tab -----------------------------------------------------------
//
// B11 -- audio-only files and files with embedded sound share the same audio
// tab. Picture capability is deliberately irrelevant to this filter.
inline bool CarriesAudio(const LibraryMedia& media) { return media.has_audio; }

// Filters `library` down to audio-capable entries within one bin (or all
// bins) and matching a case-insensitive substring `search` against
// filename/codec/path, mirroring the Media tab's existing filter (see
// main.mm's -rebuildMediaList) so the two tabs behave identically. Returns
// pointers into `library`; the caller must keep `library` alive.
inline std::vector<const LibraryMedia*> FilterAudioSources(
    const std::vector<LibraryMedia>& library, bool anyBin, bool wantRoot,
    const Ulid& wantBinId, const std::string& search) {
    const std::string searchLower = detail::Lower(search);
    std::vector<const LibraryMedia*> result;
    for (const LibraryMedia& media : library) {
        if (!CarriesAudio(media)) continue;
        if (!detail::MatchesBin(media.bin_id, anyBin, wantRoot, wantBinId))
            continue;
        if (!searchLower.empty() &&
            !detail::ContainsInsensitive(media.filename, searchLower) &&
            !detail::ContainsInsensitive(media.codec, searchLower) &&
            !detail::ContainsInsensitive(media.path, searchLower))
            continue;
        result.push_back(&media);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const LibraryMedia* left, const LibraryMedia* right) {
                         return left->filename < right->filename;
                     });
    return result;
}

// ---- Captions tab ---------------------------------------------------------

// A short, human-readable one-line description of a caption style's visual
// properties (CaptionStyle has no display `name` of its own -- see
// Document.h). Deterministic and locale-independent (ASCII "pt"/"x" units)
// so it round-trips identically across runs, which the unit test checks.
inline std::string DescribeCaptionStyle(const CaptionStyle& style) {
    std::string position = style.position;
    // ITW4 -- a custom vertical position replaces the preset.
    if (style.vertical_position.num > 0 && style.vertical_position.den > 0)
        position = std::to_string(style.vertical_position.num) + "/" +
                   std::to_string(style.vertical_position.den) + " hauteur";
    std::string description = style.font_family + " " +
                              std::to_string(style.font_size) + "pt · " +
                              position + " · " + style.color;
    if (style.font_weight != 400)
        description += " · graisse " + std::to_string(style.font_weight);
    // PA4 -- docs/roadmap/PARITE-2026-11.md.
    if (style.word_animation == kCaptionWordAnimationHighlight)
        description += " · mot dit en " + style.highlight_color;
    else if (style.word_animation == kCaptionWordAnimationReveal)
        description += " · mots révélés un à un";
    if (style.outline_width > 0)
        description += " · contour " + std::to_string(style.outline_width) +
                       " " + style.outline_color;
    if (style.shadow_offset > 0)
        description += " · ombre " + std::to_string(style.shadow_offset);
    if (!style.background_color.empty())
        description += " · fond " + style.background_color;
    return description;
}

struct CaptionStyleSummary {
    Ulid style_id;
    std::string description;
    // Cartons -- clips on caption tracks -- wearing this style as their
    // caption_group_id. See Document.h's CaptionStyle comment: a caption
    // group IS a caption_styles entry, clips join it by setting
    // DocumentClip::caption_group_id to the style's id.
    int32_t carton_count = 0;
    // F2.3 -- other clips carrying it: a picture styled before #12, or
    // through set_clip_caption, where the style shows nothing. Counted
    // apart so they neither pass for cartons nor keep the style from being
    // deleted (CaptionStyleRemoval).
    int32_t other_clip_count = 0;
};

// One entry per sequence.caption_styles, in that vector's existing order
// (append order -- the same order AddCaptionStyleOperation/
// RemoveCaptionStyleOperation already maintain), each annotated with how
// many cartons, and how many other clips, currently belong to that caption
// group.
inline std::vector<CaptionStyleSummary> SummarizeCaptionStyles(
    const DocumentSequence& sequence) {
    std::vector<CaptionStyleSummary> result;
    result.reserve(sequence.caption_styles.size());
    for (const CaptionStyle& style : sequence.caption_styles) {
        CaptionStyleSummary summary;
        summary.style_id = style.id;
        summary.description = DescribeCaptionStyle(style);
        for (const DocumentTrack& track : sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.caption_group_id == style.id) {
                    if (track.kind == "caption")
                        ++summary.carton_count;
                    else
                        ++summary.other_clip_count;
                }
        result.push_back(std::move(summary));
    }
    return result;
}

// Builds the exact SetClipCaptionOperation that joins `clipId` into
// `styleId`'s caption group, keeping whatever caption_text the caller
// already has for that clip (an empty string starts a fresh, untexted
// caption on that clip). The caller applies this via
// EditLog::Apply(document, Operation{op}, ...) -- this function never
// touches a Document.
//
// Named without an "Operation" suffix on purpose: tests/architecture_tests.py
// treats any bare `[A-Z]...Operation` token appearing in main.mm as a claim
// that a struct by that name is registered in Operations.h's variant, and
// this is a factory *function* returning an already-registered struct
// (SetClipCaptionOperation), not a new struct of its own.
inline SetClipCaptionOperation JoinClipToCaptionStyle(
    const Ulid& clipId, const Ulid& styleId, const std::string& captionText) {
    SetClipCaptionOperation operation;
    operation.clip_id = clipId;
    operation.caption_group_id = styleId;
    operation.caption_text = captionText;
    return operation;
}

// Builds the exact SetClipCaptionOperation that removes `clipId` from
// whatever caption group it belongs to (Document.h: an empty
// caption_group_id means "no caption"). See the naming note above.
inline SetClipCaptionOperation ClearClipCaption(const Ulid& clipId) {
    SetClipCaptionOperation operation;
    operation.clip_id = clipId;
    return operation;
}

// F2.3, rebranched after the visual QA of 19 September 2026. A caption is
// a carton on a caption track: the monitor (ActiveSubtitles) and the export
// draw nothing else. The tab used to join whatever clip was selected to a
// style, a video clip included, where it showed nothing, and cleared a
// carton's text along with its style, which validation refuses. It now
// dresses cartons only, and keeps their text.
struct CaptionSelection {
    // Selected clips on caption tracks, in selection order.
    std::vector<Ulid> cartons;
    // Selected clips no style can dress: pictures, sound, motion.
    size_t other_clips = 0;
    // Cartons already in the style asked about.
    size_t in_style = 0;
};

inline CaptionSelection DescribeCaptionSelection(
    const Document& document, const std::vector<Ulid>& selected,
    const Ulid& styleId) {
    CaptionSelection result;
    for (const Ulid& id : selected) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track) continue;
        if (track->kind != "caption") {
            ++result.other_clips;
            continue;
        }
        result.cartons.push_back(id);
        if (!styleId.empty() && clip->caption_group_id == styleId)
            ++result.in_style;
    }
    return result;
}

// One undo step: each of `cartons` takes `styleId` -- empty gives it back
// the default look -- and keeps its text. Cartons already there, and clips
// that are not cartons, are left out; nothing to change, or more than a
// batch holds, gives nothing.
inline std::optional<BatchOperation> RestyleCartons(
    const Document& document, const std::vector<Ulid>& cartons,
    const Ulid& styleId) {
    BatchOperation batch;
    batch.label = styleId.empty() ? "Légendes : style par défaut"
                                  : "Légendes : appliquer un style";
    for (const Ulid& id : cartons) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track || track->kind != "caption" ||
            clip->caption_group_id == styleId)
            continue;
        batch.operations.push_back(
            JoinClipToCaptionStyle(id, styleId, clip->caption_text));
    }
    if (batch.operations.empty() ||
        batch.operations.size() > kMaxBatchOperations)
        return std::nullopt;
    return batch;
}

// D7 -- docs/roadmap/DIALOGUE-2026-10.md. The words of a carton, corrected
// in the window. The engine already had the edit (SetClipCaptionOperation,
// set_clip_caption); what is decided here is what the window may send it.
//
// What was typed, made into a text every reader of a carton takes. Line
// ends become '\n' (a pasted text brings "\r\n"). A blank line goes: it ends
// a cue in the SRT a timeline is delivered as, so the rest of the carton
// would be lost on the way. A tab is a space and no other control character
// stays (the export refuses NUL). Spaces around a line go: centred, they
// would push the words aside. Bytes above ASCII pass untouched, the
// no-break space French punctuation needs among them.
inline std::string CartonTextFromTyped(const std::string& typed) {
    std::string text;
    std::string line;
    const auto endLine = [&] {
        const size_t first = line.find_first_not_of(' ');
        if (first != std::string::npos) {
            const size_t last = line.find_last_not_of(' ');
            if (!text.empty()) text.push_back('\n');
            text.append(line, first, last - first + 1);
        }
        line.clear();
    };
    for (const char byte : typed) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value == '\n' || value == '\r')
            endLine();
        else if (value == '\t')
            line.push_back(' ');
        else if (value >= 0x20 && value != 0x7f)
            line.push_back(byte);
    }
    endLine();
    return text;
}

enum class CartonTextOutcome {
    // Not a clip of a caption track: nothing else is drawn as a caption.
    NotACarton,
    // Its track is locked; the engine would refuse the edit.
    Locked,
    // Nothing would be left. Validation refuses an empty carton: one that
    // should say nothing is removed, not emptied.
    Empty,
    // The text it already has.
    Unchanged,
    Edit,
};

// Whether the window may ask for this clip's text at all: NotACarton,
// Locked, or Edit when it may.
inline CartonTextOutcome CartonTextAccess(const Document& document,
                                          const Ulid& clipId) {
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    if (!track || track->kind != "caption" || !document.FindClip(clipId))
        return CartonTextOutcome::NotACarton;
    return track->locked ? CartonTextOutcome::Locked : CartonTextOutcome::Edit;
}

struct CartonTextEdit {
    CartonTextOutcome outcome = CartonTextOutcome::NotACarton;
    // For Edit only. The style is the one the carton wears: the operation
    // writes style and text together, and a text sent alone would give the
    // carton back the default look.
    SetClipCaptionOperation operation;
};

// The one edit that gives a carton the text typed for it. A text left as it
// was makes no edit, even where CartonTextFromTyped would tidy it: saving
// what one did not touch changes nothing.
inline CartonTextEdit EditCartonText(const Document& document,
                                     const Ulid& clipId,
                                     const std::string& typed) {
    CartonTextEdit result;
    result.outcome = CartonTextAccess(document, clipId);
    if (result.outcome != CartonTextOutcome::Edit) return result;
    const DocumentClip* clip = document.FindClip(clipId);
    if (typed == clip->caption_text) {
        result.outcome = CartonTextOutcome::Unchanged;
        return result;
    }
    const std::string text = CartonTextFromTyped(typed);
    if (text.empty()) {
        result.outcome = CartonTextOutcome::Empty;
        return result;
    }
    if (text == clip->caption_text) {
        result.outcome = CartonTextOutcome::Unchanged;
        return result;
    }
    result.operation =
        JoinClipToCaptionStyle(clipId, clip->caption_group_id, text);
    return result;
}

// The carton a key opens for its text: the one clip selected, when it is a
// carton. Several clips, or a picture, give nothing -- a text belongs to
// one carton.
inline std::optional<Ulid> SelectedCartonForText(
    const Document& document, const std::vector<Ulid>& selected) {
    if (selected.size() != 1) return std::nullopt;
    const DocumentTrack* track = document.FindTrackForClip(selected.front());
    if (!track || track->kind != "caption") return std::nullopt;
    return selected.front();
}

// The words the Inspector's field holds while they are typed, shown by the
// monitor in place of the carton's own until they are committed or given
// up. Never written to the document.
struct CartonTextPreview {
    Ulid clip_id;
    std::string text;
};

// The text the monitor draws `clip` with: what is being typed for it, as
// the document would hold it (CartonTextFromTyped), and its own otherwise.
// A field emptied shows the carton's own words: an empty carton is not a
// state the document can take.
inline std::string DisplayedCartonText(
    const DocumentClip& clip, const std::optional<CartonTextPreview>& preview) {
    if (!preview || preview->clip_id != clip.id) return clip.caption_text;
    const std::string typed = CartonTextFromTyped(preview->text);
    return typed.empty() ? clip.caption_text : typed;
}

// The carton before or after this one on its track, by where it starts:
// what the Inspector's two buttons, and Tab in its field, go to. Nothing at
// either end of the track, or for a clip that is no carton. A locked track
// is still walked: its cartons are read, not written.
inline std::optional<Ulid> AdjacentCarton(const Document& document,
                                          const Ulid& clipId, bool forward) {
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    const DocumentClip* from = document.FindClip(clipId);
    if (!track || !from || track->kind != "caption") return std::nullopt;
    const DocumentClip* best = nullptr;
    for (const DocumentClip& clip : track->clips) {
        if (forward ? !(from->timeline_in < clip.timeline_in)
                    : !(clip.timeline_in < from->timeline_in))
            continue;
        if (best && (forward ? !(clip.timeline_in < best->timeline_in)
                             : !(best->timeline_in < clip.timeline_in)))
            continue;
        best = &clip;
    }
    if (!best) return std::nullopt;
    return best->id;
}

// F2.3 -- deleting a style from the tab, as one undo step. Refused
// (nothing) while a carton wears it: the tab says how many, and those
// cartons need another style first. Clips that are not cartons lose it in
// the same step, their text kept: the style never showed on them, and
// before this the tab counted them as cartons and could not delete the
// style, nor take it off them. A style nothing wears is removed alone.
inline std::optional<Operation> CaptionStyleRemoval(const Document& document,
                                                    const Ulid& styleId) {
    if (document.FindCaptionStyle(styleId) == nullptr) return std::nullopt;
    BatchOperation batch;
    batch.label = "Légendes : supprimer un style";
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips) {
            if (clip.caption_group_id != styleId) continue;
            if (track.kind == "caption") return std::nullopt;
            batch.operations.push_back(
                JoinClipToCaptionStyle(clip.id, Ulid{}, clip.caption_text));
        }
    if (batch.operations.empty())
        return Operation{RemoveCaptionStyleOperation{styleId}};
    if (batch.operations.size() + 1 > kMaxBatchOperations) return std::nullopt;
    batch.operations.push_back(RemoveCaptionStyleOperation{styleId});
    return Operation{batch};
}

// F2.3 -- what the tab sets on a style: its font, its size and the colour
// of its text. Outline, shadow, background and position stay as stored,
// and stay reachable through update_caption_style.
struct CaptionStyleLook {
    std::string font_family;
    int32_t font_size = 0;
    std::string color;
};

// The tab's size bounds, in pixels at the sequence resolution like
// font_size itself: 8 is already hard to read on a 1080-line picture, 400
// fills a third of it.
inline constexpr int32_t kCaptionTabMinimumFontSize = 8;
inline constexpr int32_t kCaptionTabMaximumFontSize = 400;

// A colour well's choice as the document spells a colour ("#rrggbb"),
// keeping the stored colour's opacity: "white@0.8" becomes "#ff0000@0.8",
// FFmpeg's colour syntax, which the caption renderer reads
// (ParseCaptionColor).
inline std::string CaptionColorFromWell(const std::string& stored, uint8_t red,
                                        uint8_t green, uint8_t blue) {
    static const char kHex[] = "0123456789abcdef";
    std::string color = "#";
    for (const uint8_t channel : {red, green, blue}) {
        color.push_back(kHex[channel >> 4U]);
        color.push_back(kHex[channel & 0x0FU]);
    }
    const size_t alpha = stored.find('@');
    if (alpha != std::string::npos) {
        color += stored.substr(alpha);
    } else if (stored.size() == 9 && stored[0] == '#') {
        // F2.3 -- "#rrggbbaa", the other way FFmpeg spells an opacity (a
        // background box often has one): its last byte is kept too.
        color += stored.substr(7);
    }
    return color;
}

// The UpdateCaptionStyleOperation that gives `styleId` this look, every
// other setting kept. Nothing, with `refusal` empty, when the look is the
// stored one: an unchanged control must not leave an undo step. Nothing,
// with `refusal` saying why, for an unknown style or a look the tab does
// not accept.
// F2.3 -- ROADMAP.md. The UpdateCaptionStyleOperation that gives the style
// `wanted.id` names every setting of `wanted`: what the Inspector writes,
// as Resolve's Track Style of a subtitle track sets the whole look at once
// (DaVinci Resolve 21.1 manual, chapter 59, page 1237). Nothing, with
// `refusal` empty, when `wanted` is the stored style: an unchanged control
// leaves no undo step. Nothing, with `refusal` in French, for an unknown
// style, a size outside the tab's bounds, or a style the engine would
// refuse -- its bounds are Document.h's (ValidateCaptionStyle), read here,
// never kept a second time.
inline std::optional<UpdateCaptionStyleOperation> RestyleCaptionStyleWhole(
    const DocumentSequence& sequence, const CaptionStyle& wanted,
    std::string& refusal) {
    refusal.clear();
    const CaptionStyle* stored = nullptr;
    for (const CaptionStyle& style : sequence.caption_styles)
        if (style.id == wanted.id) stored = &style;
    if (stored == nullptr) {
        refusal = "ce style n’existe plus";
        return std::nullopt;
    }
    if (wanted.font_family.empty() || wanted.font_family.size() > 128) {
        refusal = "police invalide";
        return std::nullopt;
    }
    if (wanted.font_size < kCaptionTabMinimumFontSize ||
        wanted.font_size > kCaptionTabMaximumFontSize) {
        refusal = "taille hors de " +
                  std::to_string(kCaptionTabMinimumFontSize) + " à " +
                  std::to_string(kCaptionTabMaximumFontSize) + " px";
        return std::nullopt;
    }
    if (wanted.color.empty() || wanted.color.size() > 64) {
        refusal = "couleur invalide";
        return std::nullopt;
    }
    const auto outside = [&](const char* what, int32_t value, int32_t maximum) {
        if (value >= 0 && value <= maximum) return false;
        refusal = std::string(what) + " hors de 0 à " +
                  std::to_string(maximum) + " px";
        return true;
    };
    if (outside("contour", wanted.outline_width,
                kCaptionStyleMaxOutlineWidth) ||
        outside("ombre", wanted.shadow_offset, kCaptionStyleMaxShadowOffset) ||
        outside("marge du fond", wanted.background_padding,
                kCaptionStyleMaxBackgroundPadding))
        return std::nullopt;
    std::string invalid;
    if (!ValidateCaptionStyle(wanted, invalid)) {
        refusal = "réglage refusé par le moteur (" + invalid + ")";
        return std::nullopt;
    }
    // Compared as the journal writes them: every setting of the style,
    // whatever is added to it later (OperationFormat.h).
    if (SerializeOperation(Operation{UpdateCaptionStyleOperation{*stored}}) ==
        SerializeOperation(Operation{UpdateCaptionStyleOperation{wanted}}))
        return std::nullopt;
    return UpdateCaptionStyleOperation{wanted};
}

inline std::optional<UpdateCaptionStyleOperation> RestyleCaptionStyle(
    const DocumentSequence& sequence, const Ulid& styleId,
    const CaptionStyleLook& look, std::string& refusal) {
    refusal.clear();
    const CaptionStyle* stored = nullptr;
    for (const CaptionStyle& style : sequence.caption_styles)
        if (style.id == styleId) stored = &style;
    if (stored == nullptr) {
        refusal = "ce style n’existe plus";
        return std::nullopt;
    }
    CaptionStyle wanted = *stored;
    wanted.font_family = look.font_family;
    wanted.font_size = look.font_size;
    wanted.color = look.color;
    return RestyleCaptionStyleWhole(sequence, wanted, refusal);
}

// F2.3 -- where the cartons of a style stand, as the Inspector offers it:
// the three presets the renderer knows, or a height of the engine's own,
// the centre of the block as an exact fraction of the frame
// (CaptionStyle::vertical_position). Resolve sets a Position X and Y; the
// engine centres the block across, so only the height is a setting.
enum class CaptionPlacement : uint8_t { Bottom, Center, Top, Custom };

// The custom height's bounds in hundredths of the frame: the engine wants
// a fraction strictly inside it (ValidateCaptionStyle).
inline constexpr int32_t kCaptionHeightPercentMin = 1;
inline constexpr int32_t kCaptionHeightPercentMax = 99;

inline CaptionPlacement CaptionPlacementOf(const CaptionStyle& style) {
    if (style.vertical_position.num > 0) return CaptionPlacement::Custom;
    if (style.position == "top") return CaptionPlacement::Top;
    if (style.position == "center") return CaptionPlacement::Center;
    return CaptionPlacement::Bottom;
}

// The height the slider shows, in hundredths of the frame: the custom one
// to the nearest, or about where a preset puts a line's centre on a
// 1080-line picture (CaptionLayout.cc keeps a twentieth of the height as
// margin), for a drag to start from.
inline int32_t CaptionHeightPercent(const CaptionStyle& style) {
    const EffectParamValue& vertical = style.vertical_position;
    if (vertical.num > 0 && vertical.den > 0) {
        const int64_t rounded = ((int64_t{200} * vertical.num) + vertical.den) /
                                (int64_t{2} * vertical.den);
        return static_cast<int32_t>(std::clamp<int64_t>(
            rounded, kCaptionHeightPercentMin, kCaptionHeightPercentMax));
    }
    switch (CaptionPlacementOf(style)) {
        case CaptionPlacement::Top:
            return 10;
        case CaptionPlacement::Center:
            return 50;
        default:
            return 90;
    }
}

// `style` placed as chosen. A preset forgets the custom height; Custom
// takes `percent` (clamped to the bounds) as an exact fraction, reduced:
// 75 is 3/4.
inline CaptionStyle WithCaptionPlacement(CaptionStyle style,
                                         CaptionPlacement placement,
                                         int32_t percent) {
    switch (placement) {
        case CaptionPlacement::Top:
            style.position = "top";
            style.vertical_position = {0, 1};
            break;
        case CaptionPlacement::Center:
            style.position = "center";
            style.vertical_position = {0, 1};
            break;
        case CaptionPlacement::Bottom:
            style.position = "bottom";
            style.vertical_position = {0, 1};
            break;
        case CaptionPlacement::Custom: {
            const int32_t height = std::clamp(percent, kCaptionHeightPercentMin,
                                              kCaptionHeightPercentMax);
            const int32_t divisor = std::gcd(height, 100);
            style.vertical_position = {height / divisor, 100 / divisor};
            break;
        }
    }
    return style;
}

// F2.3 -- the box a style's cartons are drawn on, turned on or off. On, a
// style without one gets a black box at 60 % -- FFmpeg's colour syntax, as
// the renderer reads it -- and, without a margin, a quarter of its size.
inline constexpr const char* kCaptionDefaultBackground = "#000000@0.6";

inline CaptionStyle WithCaptionBackground(CaptionStyle style, bool on) {
    if (!on) {
        style.background_color.clear();
        return style;
    }
    if (style.background_color.empty())
        style.background_color = kCaptionDefaultBackground;
    if (style.background_padding == 0)
        style.background_padding = std::clamp(
            style.font_size / 4, 1, kCaptionStyleMaxBackgroundPadding);
    return style;
}

// G3 -- docs/roadmap/GRADE-2026-09.md. The colour inputs the media menu
// offers, in the names set_media_color_input takes: the gamut/transfer pairs
// the pinned OpenColorIO config reads, the project's own first. Matrix and
// range stay "auto" -- the file's own tags -- as ingest declares them; a
// mis-tagged file is corrected through set_media_color_input.
struct ColorInputChoice {
    const char* label;
    MediaColorInput input;
};

inline const std::vector<ColorInputChoice>& ColorInputChoices() {
    static const std::vector<ColorInputChoice> choices = {
        {"Celui du projet", {}},
        {"Sony S-Log3 / S-Gamut3.Cine",
         {"sony_sgamut3_cine", "sony_slog3", "auto", "auto"}},
        {"Sony S-Log3 / S-Gamut3",
         {"sony_sgamut3", "sony_slog3", "auto", "auto"}},
        {"Rec.709", {"rec709", "rec709", "auto", "auto"}},
        {"HLG / Rec.2020", {"rec2020", "hlg", "auto", "auto"}},
        {"Rec.709 / Rec.2020", {"rec2020", "rec709", "auto", "auto"}},
        {"Linéaire Rec.709", {"rec709", "linear", "auto", "auto"}},
        {"Linéaire Rec.2020", {"rec2020", "linear", "auto", "auto"}},
        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A logo or a
        // still, placed as authored.
        {"Graphisme (logo, image)", {"", "", "auto", "auto", true}},
    };
    return choices;
}

// The choice a media's declaration is, by gamut and transfer: 0, the
// project's, when it declares nothing.
inline size_t ColorInputChoiceOf(const LibraryMedia& media) {
    const std::vector<ColorInputChoice>& choices = ColorInputChoices();
    for (size_t index = 0; index < choices.size(); ++index)
        if (choices[index].input.graphics == media.color_input.graphics &&
            choices[index].input.gamut == media.color_input.gamut &&
            choices[index].input.transfer == media.color_input.transfer)
            return index;
    return 0;
}

// One undo step setting each media's input: nothing for a media that is
// missing, has no picture, or already reads so; a batch when several
// change. Nullopt when none does.
inline std::optional<Operation> BuildSetMediaColorInputs(
    const Document& document,
    const std::vector<std::pair<Ulid, MediaColorInput>>& inputs) {
    std::vector<Operation> operations;
    for (const auto& [mediaId, input] : inputs) {
        const LibraryMedia* media = document.FindLibraryMedia(mediaId);
        if (media == nullptr || (media->metadata_complete && !media->has_video))
            continue;
        const MediaColorInput wanted =
            DeclaresColorInput(input) || input.graphics ? input
                                                        : MediaColorInput{};
        if (media->color_input == wanted) continue;
        operations.push_back(SetMediaColorInputOperation{mediaId, wanted});
    }
    if (operations.empty()) return std::nullopt;
    if (operations.size() == 1) return operations.front();
    return Operation{
        BatchOperation{"entrée couleur des médias", std::move(operations)}};
}

// PRD37 -- ROADMAP.md. The line under the media browser that says what the
// media tasks are doing, in French. The engine names a task's lifecycle in
// English in its detail ("Starting", "Complete", "Failed":
// MediaTaskManager.cc), and the QA of 3 October 2026 read "terminée · Th…ov
// · Complete" there. The state is said once, from this table; the engine's
// word for it never shows. What a running task reports of itself (the file
// it reads, a step) still does, and a failure says its error.
struct MediaTaskLine {
    std::string text;
    // A bar while the task waits or runs; none once it is over, rather
    // than a bar left at its last value.
    bool shows_progress = false;
    // 0..1.
    double progress = 0.0;
    bool cancellable = false;
};

inline const char* MediaTaskStateLabel(MediaTaskState state) {
    switch (state) {
        case MediaTaskState::Queued:
            return "en attente";
        case MediaTaskState::Running:
            return "en cours";
        case MediaTaskState::Succeeded:
            return "terminée";
        case MediaTaskState::Failed:
            return "échec";
        case MediaTaskState::Cancelled:
            return "annulée";
    }
    return "en attente";
}

// The engine's own words for where a task is in its life -- what the state
// label says already.
inline bool IsMediaTaskLifecycleDetail(const std::string& detail) {
    static const std::set<std::string> kLifecycle{
        "",       "Starting",  "Complete",
        "Failed", "Cancelled", "Cancelled before start"};
    return kLifecycle.count(detail) != 0;
}

// The task the line speaks of: the latest one waiting or running, else the
// latest one. Null when there is none.
inline const MediaTaskSnapshot* ShownMediaTask(
    const std::vector<MediaTaskSnapshot>& tasks) {
    for (auto task = tasks.rbegin(); task != tasks.rend(); ++task)
        if (task->state == MediaTaskState::Queued ||
            task->state == MediaTaskState::Running)
            return &*task;
    return tasks.empty() ? nullptr : &tasks.back();
}

inline MediaTaskLine DescribeMediaTask(const MediaTaskSnapshot* task) {
    MediaTaskLine line;
    if (task == nullptr) {
        line.text = "Tâches média : prêt";
        return line;
    }
    const bool live = task->state == MediaTaskState::Queued ||
                      task->state == MediaTaskState::Running;
    line.text = MediaTaskStateLabel(task->state);
    const auto add = [&](const std::string& part) {
        if (!part.empty()) line.text += " · " + part;
    };
    add(task->label);
    if (task->state == MediaTaskState::Failed)
        add(task->error);
    else if (live && !IsMediaTaskLifecycleDetail(task->detail))
        add(task->detail);
    line.shows_progress = live;
    line.progress = live ? std::clamp(task->progress, 0.0, 1.0) : 0.0;
    line.cancellable = live;
    return line;
}

// DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md (case 123). The Inspector
// said nothing of a dip through a colour: a clip opening on black showed
// no sign of it, and its colour was read only in the document. A dip is
// not posed with the mouse (decided on 23 October 2026, add_transition and
// --apply-op do it), but the Inspector now says, for the clip it shows,
// each dip that touches it: its side, its colour and its length in frames
// of the sequence. Black and white are named, any other colour is given as
// the document spells it.
inline std::string DipColorPhrase(const std::string& color) {
    std::string lower = color;
    for (char& character : lower)
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    if (lower == "#000000") return "au noir";
    if (lower == "#ffffff") return "au blanc";
    return "par " + color;
}

inline std::vector<std::string> ClipDipDescriptions(const Document& document,
                                                    const Ulid& clipId) {
    std::vector<std::string> lines;
    const MediaRate rate = document.sequence.frame_rate;
    for (const DocumentTransition& transition : document.sequence.transitions) {
        if (transition.type != "dip_to_color") continue;
        const bool left = transition.left_clip_id == clipId;
        const bool right = transition.right_clip_id == clipId;
        if (!left && !right) continue;
        std::string kind;
        if (transition.left_clip_id.empty())
            kind = "Ouverture";
        else if (transition.right_clip_id.empty())
            kind = "Fermeture";
        else
            kind = left ? "Fondu vers le plan suivant"
                        : "Fondu depuis le plan précédent";
        int64_t frames = 0;
        try {
            frames = transition.duration.to_frames(rate.num, rate.den);
        } catch (const std::exception&) {
            frames = 0;
        }
        lines.push_back(kind + " " + DipColorPhrase(transition.color) + " · " +
                        std::to_string(frames) +
                        (frames > 1 ? " images" : " image"));
    }
    return lines;
}

// DEC42 -- case 134. Nothing in the window said that a sequence wears a
// look (R6): the Inspector now names its .cube and the space it was
// declared for. A display-space LUT stays refused on an HLG delivery, as
// decided; that refusal is the engine's, said where it is posed.
inline std::string SequenceLookDescription(const DocumentSequence& sequence) {
    if (sequence.look_lut.digest.empty() && sequence.look_lut.name.empty())
        return "Aucun";
    std::string text = sequence.look_lut.name.empty() ? "LUT sans nom"
                                                      : sequence.look_lut.name;
    if (!sequence.look_lut.space.empty())
        text += " · espace " + sequence.look_lut.space;
    return text;
}

}  // namespace ui::media_panel
