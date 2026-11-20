#pragma once

// CH5 -- docs/roadmap/CHARTE-2026-11.md. The fields of the Charte panel and
// its undo, as policy. A field is one path of the charter (MotionKitEdit.h):
// what it shows, where its value comes from, and the edit a typed value
// becomes. The panel draws the fields it is given and writes the edit back
// as one version (DEC61: on a field's validation, never on each key).
//
// The origin a field shows matters because a charter now speaks at two
// levels (DEC62): a value for every format, and one for this format only.
// A field says which one holds the value shown -- "charte · 9:16",
// "charte", or "pièce" when the charter says nothing and the piece's own
// value shows -- so that nobody edits 16:9 believing they edit 9:16.
//
// Undo is not the EditLog's: a charter is not part of the project
// document. Cmd+Z in the panel restores the version the last gesture
// replaced, as a new version (DEC61), and the history here only remembers
// which versions those were.
//
// Portable: no AppKit, no I/O (tests/charter_panel_model_tests.cc).

#include "CharterPreview.h"
#include "Json.h"
#include "MotionKitEdit.h"
#include "MotionTemplates.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ui::charter {

enum class FieldKind {
    Color,
    Number,
    Integer,
    Enum,
    Text,
    LongText,
    List,
    Bool,
};

// Where a field's value comes from, as the panel prints it.
inline constexpr char kOriginCharter[] = "charte";
inline constexpr char kOriginPiece[] = "pièce";
// "charte · " followed by the frame's label: "charte · 9:16".
inline constexpr char kOriginCharterFormatPrefix[] = "charte · ";
// A caption setting the charter's captions leave out: the value shown is
// the one a new caption style starts from (CaptionStyle, Document.h).
inline constexpr char kOriginDefault[] = "défaut";

struct Field {
    // The charter path an edit of this field writes (MotionKitEdit.h).
    std::string path;
    // French, as the panel shows it: "Couleur d'accent", "Marge gauche".
    std::string label;
    // The group the panel shows it under, French: "Charte", "Couleurs",
    // "Polices", "Sous-titres", "Pièce · logo".
    std::string section;
    FieldKind kind = FieldKind::Text;
    // Number and Integer: the bounds the value is clamped to, when there are
    // any, and the step it is rounded to (0: none).
    std::optional<double> min;
    std::optional<double> max;
    double step = 0.0;
    // Enum: the values, as the piece declares them.
    std::vector<std::string> options;
    // The value shown, as typed: a number as MotionKitNumber spells it, a
    // colour as written, "true" or "false", a list one item per line. Empty
    // when nothing gives one.
    std::string value;
    // kOriginCharterFormatPrefix + label, kOriginCharter or kOriginPiece;
    // kOriginDefault for a caption setting the captions leave out; empty
    // when nothing gives a value.
    std::string origin;
};

// The fields of `kitFile` (the charter's JSON tree) for the preview frame
// `frame`, in display order, one section after the other: "Charte"
// (description, principle, sources); "Couleurs" (the shared roles
// props.accent, texte, fond, then the palette); "Polices" (props.police,
// then fonts) -- the lists one item per line; "Sous-titres", the caption
// settings, at `captions.formats.<f>.<setting>` when the charter has a
// caption format of that size and the value is set there,
// `captions.<setting>` otherwise -- the vertical position as a whole
// percentage, 1 to 99 -- and, for each piece of `pieces` the charter does
// not withdraw, "Pièce · <p>", its own props (no group, no list, no point,
// no size) with their declared type, bounds and values -- a text prop only
// when it is a shared role or the file an image node's `src` is bound to
// (the logo's `logo`): the name, the function, a title are the content of
// one film, not values of the brand (CH6). A prop named as a
// shared role (accent, police...) is left to the role's field, unless the
// charter sets it for that piece. A piece prop's field writes
// `pieces.<p>.formats.<f>.<prop>` when its value already differs by format
// -- the charter sets it for that format, or the piece's own format does --
// and `pieces.<p>.<prop>` otherwise. Empty when `kitFile` is not an object
// (a broken charter is shown as its JSON, not as fields).
std::vector<Field> KitFields(const mcp_json::Value& kitFile,
                             const std::vector<MotionTemplateInfo>& pieces,
                             const PreviewFrame& frame);

// `typedText` as the edit of `field`. An empty text removes the value
// (back to what the level below gives); a number is rounded to the field's
// step and clamped to its bounds (MotionKitNumber); an integer is whole;
// the vertical position becomes a reduced fraction {"num", "den"}; a
// colour is checked as the path takes it ("#rrggbb" in the palette, with
// "@a" in the captions, any colour of the motion format elsewhere); a list
// is one item per non-empty line; a bool is "true" or "false". False with
// `messageFr`, a sentence in French for the field's error line, when the
// text is not what the field takes. Whether the charter then still reads
// is EditMotionKit's to say.
bool FieldEdit(const Field& field, const std::string& typedText,
               MotionKitEdit& edit, std::string& messageFr);

// `edits`, preceded by `format: "brand-kit.v1"` when `kitFile` is not
// already a brand's charter and one of them writes a field only a brand's
// charter has (palette, fonts, sources, captions, pieces, withdrawn):
// EditMotionKit refuses such an edit on a motion-kit.v1 unless the same
// batch upgrades it. What the edit of a FieldEdit is sent through before
// EditMotionKit; DragEdits already is.
std::vector<MotionKitEdit> WithBrandFormat(const mcp_json::Value& kitFile,
                                           std::vector<MotionKitEdit> edits);

// A new charter: brand-kit.v1, `description`, and `props.police` set to
// "Helvetica Neue", the font every shipped piece defaults to -- a charter
// must set something, and this one changes nothing on screen until it is
// edited.
mcp_json::Value NewKitFile(const std::string& description);

// The "per format" action of the subtitle: for each of `frames` the
// charter has no caption format of, `captions.formats.<frame.name>` with
// its width and height and nothing else, so every size still shows the
// base settings. Empty when the charter has no captions, or already has a
// format of every size. The panel warns first: once a charter has caption
// formats, a size without one gets no caption style at all.
std::vector<MotionKitEdit> CaptionFormatEdits(
    const mcp_json::Value& kitFile, const std::vector<PreviewFrame>& frames);

// CH6 -- what the panel says when kits/<name>.json no longer reads as a
// charter (MotionKitFileState::Diverged, or a write refused with
// KitFileDiverged): the file is the person's work in progress, so the
// engine writes nothing -- a restore included -- until it is fixed or
// moved, and the sentence says so with the reason. `issue` is the engine's
// (MotionKitState::file_issue, or the refusal's message).
std::string KitFileDivergedMessage(const std::string& name,
                                   const std::string& issue);

// CH6 -- a write the engine refused, as a sentence in French for the
// panel's notice line, by its code (MotionKitEdit.h): StaleKit, someone
// wrote since and the panel reads the charter again; KitFileDiverged, as
// KitFileDivergedMessage; InvalidKit and InvalidEdit, nothing written and
// why; UnknownKit; IoError. The engine's message is kept as the reason.
std::string KitWriteRefusalMessage(const std::string& name,
                                   const MotionKitError& error);

// One gesture saved: version `to_version` written over `from_version`.
struct HistoryStep {
    int64_t from_version = 0;
    int64_t to_version = 0;
};

// The panel's undo for one charter, newest last. Kept per charter while
// the panel is open; another charter chosen starts a new one.
struct History {
    std::vector<HistoryStep> undo;
    std::vector<HistoryStep> redo;
};

// A gesture or a field saved as `to_version` over `from_version`: pushed on
// `undo`, `redo` cleared. Nothing when no version was written (equal
// versions).
void RecordSave(History& history, int64_t fromVersion, int64_t toVersion);

// The version whose content Cmd+Z restores, as a new version: the
// `from_version` of the last step, while that step's `to_version` is still
// the latest. Not "latest - 1": after an undo the latest is the restored
// copy, and latest - 1 would undo the undo. Nothing when the history is
// empty, or someone else (the agent, a hand edit) wrote since: the panel
// then reloads instead of undoing their work.
std::optional<int64_t> UndoTarget(const History& history,
                                  int64_t latestVersion);

// Cmd+Shift+Z: the `to_version` of the last redo step, while its
// `from_version` is the latest.
std::optional<int64_t> RedoTarget(const History& history,
                                  int64_t latestVersion);

// The undo restored as `writtenVersion`: its step moves to `redo` as
// {writtenVersion, to_version}, and the step below it, if it ended where
// this one started, now ends at `writtenVersion` (the same content).
void RecordUndo(History& history, int64_t writtenVersion);

// The redo restored as `writtenVersion`: its step moves back to `undo` as
// {from_version, writtenVersion}, and the redo step below it, if it began
// where this one ended, now begins at `writtenVersion`.
void RecordRedo(History& history, int64_t writtenVersion);

}  // namespace ui::charter
