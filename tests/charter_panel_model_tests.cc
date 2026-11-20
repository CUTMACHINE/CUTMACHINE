// CH5 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel's fields: what
// each shows, where its value comes from (the charter for this format, the
// charter for every format, the piece), the path it writes; the edit a
// typed value becomes and the French sentence of a refusal; a new charter;
// the "per format" action of the subtitle; and the undo over versions,
// which restores what the last gesture replaced, never latest - 1.
// Portable: no framework, a neutral charter.

#include "CharterPanelModel.h"
#include "CharterPreview.h"

#include "Document.h"
#include "Json.h"
#include "MotionKitEdit.h"
#include "MotionTemplates.h"
#include "TestSupport.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;
using ui::charter::Field;
using ui::charter::FieldKind;
using ui::charter::History;
using ui::charter::HistoryStep;
using ui::charter::PreviewFrames;

Value Json(const std::string& text) {
    Value value;
    std::string error;
    Check(Value::Parse(text, value, error), "test JSON parses: " + error);
    return value;
}

// A neutral charter at both levels: values for every format, and one for
// 9:16 only (DEC62), read as a tree as the panel reads the file.
const char kKitText[] =
    R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
    R"("principle":"Une barre sous chaque titre.\nJamais de dégradé.",)"
    R"("props":{"accent":"#1D6FA3","police":"Exemple Sans"},)"
    R"("palette":["#1D6FA3","#F2B705"],"fonts":["Exemple Sans"],)"
    R"("sources":["Charte d’exemple, page 4"],)"
    R"("captions":{"font_family":"Exemple Sans","font_size":56,)"
    R"("color":"#FFFFFF","vertical_position":{"num":3,"den":4},)"
    R"("formats":{"vertical":{"width":1080,"height":1920,"font_size":64}}},)"
    R"("pieces":{"logo":{"logo":"/kits/marque/logo.png","marge_droite":10,)"
    R"("formats":{"9x16":{"marge_haut":20}}},)"
    R"("nom":{"accent":"#F2B705"}},)"
    R"("withdrawn":{"carton":"pas besoin"}})";

const Field* FieldAt(const std::vector<Field>& fields,
                     const std::string& section, const std::string& label) {
    for (const Field& field : fields) {
        if (field.section == section && field.label == label) {
            return &field;
        }
    }
    return nullptr;
}

std::string Describe(const Field* field) {
    if (field == nullptr) {
        return "<no field>";
    }
    return field->path + " = \"" + field->value + "\" (" + field->origin + ")";
}

bool Is(const Field* field, const std::string& path, const std::string& value,
        const std::string& origin) {
    return field != nullptr && field->path == path && field->value == value &&
           field->origin == origin;
}

void CheckFields(const std::vector<MotionTemplateInfo>& shipped) {
    const Value kitFile = Json(kKitText);
    const std::vector<Field> vertical =
        ui::charter::KitFields(kitFile, shipped, PreviewFrames()[0]);
    const std::vector<Field> wide =
        ui::charter::KitFields(kitFile, shipped, PreviewFrames()[1]);

    // The charter's own fields, at the top.
    Check(vertical.size() > 9 && vertical[0].path == "description" &&
              vertical[0].section == "Charte" &&
              vertical[0].kind == FieldKind::Text &&
              vertical[0].value == "Charte d’exemple." &&
              vertical[0].origin == ui::charter::kOriginCharter,
          "the description comes first, from the charter");
    const Field* principle = FieldAt(vertical, "Charte", "Principe graphique");
    Check(principle != nullptr && principle->kind == FieldKind::LongText &&
              principle->value ==
                  "Une barre sous chaque titre.\nJamais de dégradé.",
          "the principle is a long text, its lines kept");
    Check(Is(FieldAt(vertical, "Charte", "Sources"), "sources",
             "Charte d’exemple, page 4", "charte"),
          "the sources, one per line: " +
              Describe(FieldAt(vertical, "Charte", "Sources")));
    Check(Is(FieldAt(vertical, "Couleurs", "Couleur d’accent"), "props.accent",
             "#1D6FA3", "charte"),
          "the accent role, as written");
    Check(Is(FieldAt(vertical, "Couleurs", "Couleur du texte"), "props.texte",
             "", ""),
          "a role the charter leaves out shows nothing, from nowhere");
    const Field* palette = FieldAt(vertical, "Couleurs", "Palette");
    Check(Is(palette, "palette", "#1D6FA3\n#F2B705", "charte") &&
              palette->kind == FieldKind::List,
          "the palette, one colour per line: " + Describe(palette));
    Check(Is(FieldAt(vertical, "Polices", "Police"), "props.police",
             "Exemple Sans", "charte"),
          "the font role");
    Check(Is(FieldAt(vertical, "Polices", "Polices permises"), "fonts",
             "Exemple Sans", "charte"),
          "the fonts list");

    // Sections follow each other, none split; a piece per section, in the
    // catalog's order, but the card the charter withdraws.
    std::vector<std::string> sections;
    for (const Field& field : vertical) {
        if (sections.empty() || sections.back() != field.section) {
            sections.push_back(field.section);
        }
    }
    const auto count = [&sections](const std::string& name) {
        return std::count(sections.begin(), sections.end(), name);
    };
    Check(sections.size() > 5 && sections[0] == "Charte" &&
              sections[1] == "Couleurs" && sections[2] == "Polices" &&
              sections[3] == "Sous-titres" && count("Pièce · logo") == 1 &&
              count("Pièce · nom") == 1 && count("Pièce · carton") == 0,
          "the sections in order, each once, the withdrawn card left out");
    bool contiguous = true;
    for (const std::string& section : sections) {
        contiguous = contiguous && count(section) == 1;
    }
    Check(contiguous, "no section is split");

    // Captions: the 9:16 format where it says something, the base
    // otherwise, the starting style where neither does.
    Check(Is(FieldAt(vertical, "Sous-titres", "Taille (px)"),
             "captions.formats.vertical.font_size", "64", "charte · 9:16"),
          "in 9:16 the size is the caption format's: " +
              Describe(FieldAt(vertical, "Sous-titres", "Taille (px)")));
    Check(Is(FieldAt(wide, "Sous-titres", "Taille (px)"), "captions.font_size",
             "56", "charte"),
          "in 16:9 the base size: " +
              Describe(FieldAt(wide, "Sous-titres", "Taille (px)")));
    Check(Is(FieldAt(vertical, "Sous-titres", "Police"), "captions.font_family",
             "Exemple Sans", "charte"),
          "a setting the format does not change writes the base");
    const Field* height = FieldAt(vertical, "Sous-titres", "Hauteur (%)");
    Check(Is(height, "captions.vertical_position", "75", "charte") &&
              height->kind == FieldKind::Integer && height->min == 1.0 &&
              height->max == 99.0,
          "the height as a whole percentage, 1 to 99: " + Describe(height));
    Check(Is(FieldAt(vertical, "Sous-titres", "Contour (px)"),
             "captions.outline_width", "0", "défaut"),
          "a setting the captions leave out shows the starting style's");
    const Field* position = FieldAt(vertical, "Sous-titres", "Position");
    Check(position != nullptr && position->kind == FieldKind::Enum &&
              position->options ==
                  std::vector<std::string>{"bottom", "center", "top"},
          "the position is one of the renderer's three");

    // The logo: 9:16 has its own top margin in the charter; the right
    // margin is the charter's for every format, and the piece's 9:16
    // format sets it too, so a 9:16 edit goes to 9:16; the width is the
    // piece's own, alike in every format.
    Check(Is(FieldAt(vertical, "Pièce · logo", "Marge haut (%)"),
             "pieces.logo.formats.9x16.marge_haut", "20", "charte · 9:16"),
          "the 9:16 top margin, from the charter for 9:16: " +
              Describe(FieldAt(vertical, "Pièce · logo", "Marge haut (%)")));
    Check(Is(FieldAt(wide, "Pièce · logo", "Marge haut (%)"),
             "pieces.logo.marge_haut", "6", "pièce"),
          "in 16:9 the charter's 9:16 value does not hold: " +
              Describe(FieldAt(wide, "Pièce · logo", "Marge haut (%)")));
    Check(Is(FieldAt(vertical, "Pièce · logo", "Marge droite (%)"),
             "pieces.logo.formats.9x16.marge_droite", "10", "charte"),
          "a value the piece's format sets is edited in that format: " +
              Describe(FieldAt(vertical, "Pièce · logo", "Marge droite (%)")));
    const Field* largeur = FieldAt(vertical, "Pièce · logo", "Largeur (%)");
    Check(Is(largeur, "pieces.logo.largeur", "18", "pièce") &&
              largeur->kind == FieldKind::Number && largeur->min == 2.0 &&
              largeur->max == 60.0 && largeur->step == 0.5,
          "a % prop: its bounds and the drag's step: " + Describe(largeur));
    Check(Is(FieldAt(wide, "Pièce · logo", "Largeur (%)"),
             "pieces.logo.formats.16x9.largeur", "10", "pièce"),
          "in 16:9 the piece's format sets the width: " +
              Describe(FieldAt(wide, "Pièce · logo", "Largeur (%)")));
    const Field* coin = FieldAt(vertical, "Pièce · logo", "Coin");
    Check(Is(coin, "pieces.logo.coin", "haut-droite", "pièce") &&
              coin->kind == FieldKind::Enum && coin->options.size() == 4,
          "the corner, its four values: " + Describe(coin));
    Check(Is(FieldAt(vertical, "Pièce · logo", "Logo"), "pieces.logo.logo",
             "/kits/marque/logo.png", "charte"),
          "the charter's logo file");

    // The name: a role the charter sets for it shows; the others are the
    // role fields' business; a group has no field.
    Check(Is(FieldAt(vertical, "Pièce · nom", "Accent"), "pieces.nom.accent",
             "#F2B705", "charte"),
          "a role set for the piece is shown in the piece");
    Check(FieldAt(vertical, "Pièce · nom", "Police") == nullptr &&
              FieldAt(vertical, "Pièce · nom", "Texte") == nullptr,
          "roles the charter does not set for the piece are not repeated");
    Check(FieldAt(vertical, "Pièce · nom", "Texte group") == nullptr,
          "a group is no field");
    const Field* corps = FieldAt(vertical, "Pièce · nom", "Corps");
    Check(corps != nullptr && corps->step == 0.0 && corps->min == 16.0 &&
              corps->max == 200.0,
          "a prop in pixels has no step but its bounds");
    Check(Is(FieldAt(vertical, "Pièce · nom", "Fond fonction"),
             "pieces.nom.fond_fonction", "#000000", "pièce"),
          "an own colour of the piece, its default");

    // CH6 -- the words of a film are not the brand's: the name and the
    // function of the lower third, the card's title and subtitle have no
    // field; the logo's file, which an image shows, and a shared role set
    // for the piece keep theirs.
    Check(FieldAt(vertical, "Pièce · nom", "Nom") == nullptr &&
              FieldAt(vertical, "Pièce · nom", "Fonction") == nullptr,
          "the lower third's name and function are content, not charter");
    const std::vector<Field> withCard = ui::charter::KitFields(
        Json(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
             R"("props":{"police":"Exemple Sans"},)"
             R"("pieces":{"carton":{"police":"Exemple Sans"}}})"),
        shipped, PreviewFrames()[0]);
    Check(FieldAt(withCard, "Pièce · carton", "Titre") == nullptr &&
              FieldAt(withCard, "Pièce · carton", "Sous titre") == nullptr,
          "the card's title and subtitle are content, not charter");
    Check(Is(FieldAt(withCard, "Pièce · carton", "Police"),
             "pieces.carton.police", "Exemple Sans", "charte"),
          "a shared text role the charter sets for the card stays a field: " +
              Describe(FieldAt(withCard, "Pièce · carton", "Police")));
    Check(Is(FieldAt(withCard, "Pièce · logo", "Logo"), "pieces.logo.logo",
             "logo.png", "pièce"),
          "the file the logo's image shows stays a field, the piece's own "
          "default when the charter names none: " +
              Describe(FieldAt(withCard, "Pièce · logo", "Logo")));

    Check(ui::charter::KitFields(Json("[1, 2]"), shipped, PreviewFrames()[0])
              .empty(),
          "a file that is not an object has no fields: it is shown raw");

    // A charter without captions offers them, empty.
    const std::vector<Field> bare = ui::charter::KitFields(
        Json(R"({"format":"motion-kit.v1","description":"Ancienne charte.",)"
             R"("props":{"accent":"#1D6FA3"}})"),
        shipped, PreviewFrames()[0]);
    Check(Is(FieldAt(bare, "Sous-titres", "Taille (px)"), "captions.font_size",
             "", ""),
          "no captions: the fields are offered, empty");
}

bool EditIs(const MotionKitEdit& edit, const std::string& path,
            const std::string& value) {
    return edit.path == path && edit.value && edit.value->Dump() == value &&
           !edit.insert;
}

void CheckFieldEdit(const std::vector<MotionTemplateInfo>& shipped) {
    const std::vector<Field> fields =
        ui::charter::KitFields(Json(kKitText), shipped, PreviewFrames()[0]);
    MotionKitEdit edit;
    std::string message;

    const Field* largeur = FieldAt(fields, "Pièce · logo", "Largeur (%)");
    Check(largeur != nullptr &&
              ui::charter::FieldEdit(*largeur, " 12,3 ", edit, message) &&
              EditIs(edit, "pieces.logo.largeur", "12.5"),
          "a number typed with a comma, rounded to the step");
    Check(ui::charter::FieldEdit(*largeur, "100", edit, message) &&
              EditIs(edit, "pieces.logo.largeur", "60"),
          "a number past the maximum is clamped to it");
    Check(!ui::charter::FieldEdit(*largeur, "douze", edit, message) &&
              message == "Un nombre est attendu, par exemple 12,5.",
          "not a number: refused, in French: " + message);
    Check(ui::charter::FieldEdit(*largeur, "  ", edit, message) &&
              edit.path == "pieces.logo.largeur" && !edit.value,
          "an empty field removes the value");

    const Field* height = FieldAt(fields, "Sous-titres", "Hauteur (%)");
    Check(
        height != nullptr &&
            ui::charter::FieldEdit(*height, "75", edit, message) &&
            EditIs(edit, "captions.vertical_position", R"({"num":3,"den":4})"),
        "the height becomes a reduced fraction");
    Check(ui::charter::FieldEdit(*height, "150", edit, message) &&
              EditIs(edit, "captions.vertical_position",
                     R"({"num":99,"den":100})"),
          "never past 99 %");
    Check(ui::charter::FieldEdit(*height, "0", edit, message) &&
              EditIs(edit, "captions.vertical_position",
                     R"({"num":1,"den":100})"),
          "never under 1 %");

    const Field* size = FieldAt(fields, "Sous-titres", "Taille (px)");
    Check(size != nullptr &&
              ui::charter::FieldEdit(*size, "64.4", edit, message) &&
              EditIs(edit, "captions.formats.vertical.font_size", "64"),
          "a caption size is a whole number of pixels");
    Check(!ui::charter::FieldEdit(*size, "grand", edit, message) &&
              message == "Un nombre entier est attendu.",
          "an integer field says so: " + message);

    const Field* captionColor = FieldAt(fields, "Sous-titres", "Couleur");
    Check(captionColor != nullptr &&
              ui::charter::FieldEdit(*captionColor, "#202020@0.5", edit,
                                     message) &&
              EditIs(edit, "captions.color", R"("#202020@0.5")"),
          "a caption colour takes an opacity");
    Check(!ui::charter::FieldEdit(*captionColor, "white", edit, message) &&
              message.find("#rrggbb@a") != std::string::npos,
          "a caption colour is #rrggbb, with or without @a: " + message);
    const Field* accent = FieldAt(fields, "Couleurs", "Couleur d’accent");
    Check(accent != nullptr &&
              ui::charter::FieldEdit(*accent, "white", edit, message) &&
              EditIs(edit, "props.accent", R"("white")"),
          "a role takes any colour of the motion format");
    Check(!ui::charter::FieldEdit(*accent, "#12", edit, message) &&
              message.find("Couleur illisible") == 0,
          "a colour that does not read is refused: " + message);

    const Field* palette = FieldAt(fields, "Couleurs", "Palette");
    Check(palette != nullptr &&
              ui::charter::FieldEdit(*palette, "#1D6FA3\n\n #F2B705 \n", edit,
                                     message) &&
              EditIs(edit, "palette", R"(["#1D6FA3","#F2B705"])"),
          "a list, one item per non-empty line");
    Check(!ui::charter::FieldEdit(*palette, "#1D6FA3\n#F2B705@0.5", edit,
                                  message) &&
              message.find("Ligne 2") == 0,
          "a palette line with an opacity is refused, by its line: " + message);

    const Field* coin = FieldAt(fields, "Pièce · logo", "Coin");
    Check(coin != nullptr &&
              ui::charter::FieldEdit(*coin, "bas-gauche", edit, message) &&
              EditIs(edit, "pieces.logo.coin", R"("bas-gauche")"),
          "an enum takes one of its values");
    Check(!ui::charter::FieldEdit(*coin, "milieu", edit, message) &&
              message ==
                  "Valeurs possibles : haut-droite, haut-gauche, "
                  "bas-droite, bas-gauche.",
          "and names them otherwise: " + message);

    const Field* principle = FieldAt(fields, "Charte", "Principe graphique");
    Check(principle != nullptr &&
              ui::charter::FieldEdit(*principle, "Une ligne.\nUne autre.\n",
                                     edit, message) &&
              EditIs(edit, "principle", R"("Une ligne.\nUne autre.")"),
          "a long text keeps its lines");
    const Field* description = FieldAt(fields, "Charte", "Description");
    Check(description != nullptr &&
              !ui::charter::FieldEdit(*description, "Une ligne.\nUne autre.",
                                      edit, message) &&
              message == "Une seule ligne est attendue.",
          "a text is one line");

    Field flag;
    flag.path = "pieces.badge.visible";
    flag.kind = FieldKind::Bool;
    Check(ui::charter::FieldEdit(flag, "true", edit, message) &&
              EditIs(edit, "pieces.badge.visible", "true"),
          "a boolean");
    Check(!ui::charter::FieldEdit(flag, "oui", edit, message) &&
              message == "Attendu : true ou false.",
          "a boolean is true or false: " + message);
}

void CheckBrandFormat() {
    const Value old =
        Json(R"({"format":"motion-kit.v1","description":"Ancienne charte.",)"
             R"("props":{"accent":"#1D6FA3"}})");
    std::vector<MotionKitEdit> edits = ui::charter::WithBrandFormat(
        old, {{"pieces.logo.coin", Value::MakeString("bas-gauche"), false}});
    Check(edits.size() == 2 &&
              EditIs(edits[0], "format", R"("brand-kit.v1")") &&
              EditIs(edits[1], "pieces.logo.coin", R"("bas-gauche")"),
          "a piece's value on a motion-kit.v1 upgrades the charter first");
    edits = ui::charter::WithBrandFormat(
        old, {{"props.accent", Value::MakeString("#202020"), false}});
    Check(edits.size() == 1, "a role stays a motion-kit.v1's");
    edits = ui::charter::WithBrandFormat(
        Json(kKitText),
        {{"captions.color", Value::MakeString("#202020"), false}});
    Check(edits.size() == 1, "a brand's charter needs no upgrade");
}

void CheckNewKit() {
    const Value file = ui::charter::NewKitFile("Nouvelle charte de marque.");
    MotionKitInfo kit;
    std::string error;
    Check(ParseMotionKit("marque", file.Dump(), kit, error),
          "a new charter reads: " + error);
    Check(kit.format == kBrandKitFormat &&
              kit.description == "Nouvelle charte de marque." &&
              kit.props.Dump() == R"({"police":"Helvetica Neue"})",
          "a brand's charter, its description, the shipped pieces' font: " +
              file.Dump());
    Check(file.AsObject().size() == 3 && file.AsObject()[0].first == "format" &&
              file.AsObject()[1].first == "description",
          "format first, as a charter is written");
}

// `file` with each edit's value set at its path, parents created.
Value Laid(Value file, const std::vector<MotionKitEdit>& edits) {
    const auto lay = [](auto& self, Value& at, const std::string& path,
                        const Value& value) -> void {
        const size_t dot = path.find('.');
        const std::string key = path.substr(0, dot);
        if (dot == std::string::npos) {
            at.Set(key, value);
            return;
        }
        const Value* existing = at.Find(key);
        Value child = existing != nullptr && existing->IsObject()
                          ? *existing
                          : Value::MakeObject();
        self(self, child, path.substr(dot + 1), value);
        at.Set(key, std::move(child));
    };
    for (const MotionKitEdit& edit : edits) {
        if (edit.value) {
            lay(lay, file, edit.path, *edit.value);
        }
    }
    return file;
}

void CheckCaptionFormats() {
    const std::vector<ui::charter::PreviewFrame>& frames = PreviewFrames();
    Check(ui::charter::CaptionFormatEdits(
              Json(R"({"format":"brand-kit.v1","description":"Charte.",)"
                   R"("props":{"accent":"#1D6FA3"}})"),
              frames)
              .empty(),
          "no captions: no caption format to create");
    const Value base =
        Json(R"({"format":"brand-kit.v1","description":"Charte.",)"
             R"("captions":{"font_family":"Exemple Sans","font_size":56}})");
    const std::vector<MotionKitEdit> edits =
        ui::charter::CaptionFormatEdits(base, frames);
    Check(edits.size() == 3 &&
              EditIs(edits[0], "captions.formats.9x16",
                     R"({"width":1080,"height":1920})") &&
              EditIs(edits[1], "captions.formats.16x9",
                     R"({"width":1920,"height":1080})") &&
              EditIs(edits[2], "captions.formats.1x1",
                     R"({"width":1080,"height":1080})"),
          "a format per size, its size and nothing else");
    // Laid in, the charter still reads, and every size shows the base.
    const Value laid = Laid(base, edits);
    MotionKitInfo kit;
    std::string error;
    Check(ParseMotionKit("marque", laid.Dump(), kit, error),
          "with its formats the charter reads: " + error);
    for (const ui::charter::PreviewFrame& frame : frames) {
        CaptionStyle style;
        std::string format;
        std::vector<std::string> set;
        Check(MotionKitCaptionStyle(kit, frame.width, frame.height, style,
                                    format, set, error) &&
                  format == frame.name && style.font_size == 56,
              "in " + frame.label + " the base settings hold: " + error);
    }
    Check(ui::charter::CaptionFormatEdits(laid, frames).empty(),
          "a format of every size: nothing more");

    const Value partial =
        Json(R"({"format":"brand-kit.v1","description":"Charte.",)"
             R"("captions":{"font_size":56,"formats":{"9x16":{"width":720,)"
             R"("height":1280},"large":{"width":1920,"height":1080}}}})");
    const std::vector<MotionKitEdit> more =
        ui::charter::CaptionFormatEdits(partial, frames);
    Check(more.size() == 2 &&
              EditIs(more[0], "captions.formats.9x16_1080x1920",
                     R"({"width":1080,"height":1920})") &&
              EditIs(more[1], "captions.formats.1x1",
                     R"({"width":1080,"height":1080})"),
          "a size already covered is skipped, a name already taken by "
          "another size is told apart by the size");
}

void CheckHistory() {
    History history;
    Check(!ui::charter::UndoTarget(history, 1) &&
              !ui::charter::RedoTarget(history, 1),
          "an empty history undoes nothing");
    ui::charter::RecordSave(history, 3, 3);
    Check(history.undo.empty(), "no version written: nothing recorded");

    // Two gestures: v1 -> v2 -> v3.
    ui::charter::RecordSave(history, 1, 2);
    ui::charter::RecordSave(history, 2, 3);
    Check(ui::charter::UndoTarget(history, 3) == 2,
          "Cmd+Z restores the content the last gesture replaced");
    Check(!ui::charter::UndoTarget(history, 4),
          "someone else wrote since: no undo over their work");

    // Undone as v4 (the content of v2), then undone again as v5.
    ui::charter::RecordUndo(history, 4);
    Check(history.undo.size() == 1 && history.undo[0].from_version == 1 &&
              history.undo[0].to_version == 4 && history.redo.size() == 1 &&
              history.redo[0].from_version == 4 &&
              history.redo[0].to_version == 3,
          "the undo moves to redo, the step below now ends at the copy");
    Check(ui::charter::UndoTarget(history, 4) == 1,
          "a second Cmd+Z goes further back, to v1 -- not to v3, latest - 1, "
          "which would undo the undo");
    ui::charter::RecordUndo(history, 5);
    Check(history.undo.empty() && !ui::charter::UndoTarget(history, 5),
          "the history is used up");

    // Redone as v6 (the content of v2), then as v7 (that of v3).
    Check(ui::charter::RedoTarget(history, 5) == 4,
          "Cmd+Shift+Z restores what the last undo replaced");
    ui::charter::RecordRedo(history, 6);
    Check(ui::charter::RedoTarget(history, 6) == 3,
          "a second redo goes on to the content of v3");
    ui::charter::RecordRedo(history, 7);
    Check(history.redo.empty() && history.undo.size() == 2 &&
              history.undo[0].from_version == 5 &&
              history.undo[0].to_version == 6 &&
              history.undo[1].from_version == 6 &&
              history.undo[1].to_version == 7,
          "both redos are undoable again");
    Check(ui::charter::UndoTarget(history, 7) == 6,
          "undo after redo restores the content of v6, which is v2's");

    // A new gesture after an undo clears the redo.
    ui::charter::RecordUndo(history, 8);
    Check(ui::charter::RedoTarget(history, 8) == 7, "one redo waits");
    ui::charter::RecordSave(history, 8, 9);
    Check(history.redo.empty() && !ui::charter::RedoTarget(history, 9) &&
              ui::charter::UndoTarget(history, 9) == 8,
          "a new save clears the redo and is the next undo");
    Check(!ui::charter::RedoTarget(History{{}, {{2, 5}}}, 3),
          "a redo whose start is not the latest is not offered");
}

// CH6 -- what the panel says of a diverged file and of a refused write.
void CheckMessages() {
    const std::string diverged = ui::charter::KitFileDivergedMessage(
        "marque", "unexpected end of input");
    Check(diverged ==
              "Le fichier kits/marque.json ne se lit plus : unexpected end of "
              "input. Corrige-le ou déplace-le ; le panneau n’écrit rien tant "
              "qu’il diverge.",
          "a diverged file: the file, the reason, and that nothing is "
          "written: " +
              diverged);
    Check(ui::charter::KitFileDivergedMessage("marque", "")
                  .find("ne se lit plus. Corrige-le") != std::string::npos,
          "without a reason, the sentence still reads");
    MotionKitError error{kMotionKitFileDiverged, "/tmp/kits/marque.json",
                         "not a regular file"};
    Check(
        ui::charter::KitWriteRefusalMessage("marque", error) ==
            ui::charter::KitFileDivergedMessage("marque", "not a regular file"),
        "a write refused on a diverged file says the same");
    error = {kMotionKitStaleKit, "", "latest is v4"};
    Check(ui::charter::KitWriteRefusalMessage("marque", error)
                  .find("a changé ailleurs") != std::string::npos,
          "a stale write: someone wrote since, the panel reads again");
    error = {kMotionKitInvalidKit, "captions.font_size", "font_size: 900"};
    const std::string invalid =
        ui::charter::KitWriteRefusalMessage("marque", error);
    Check(invalid.find("font_size: 900") != std::string::npos &&
              invalid.find("rien n’est écrit") != std::string::npos,
          "a refused value keeps the engine's reason: " + invalid);
    error = {kMotionKitIoError, "", "disk full"};
    Check(ui::charter::KitWriteRefusalMessage("marque", error)
                  .find("disk full") != std::string::npos,
          "an I/O failure keeps its reason");
}

}  // namespace

int main() {
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    CheckFields(shipped);
    CheckFieldEdit(shipped);
    CheckBrandFormat();
    CheckNewKit();
    CheckCaptionFormats();
    CheckHistory();
    CheckMessages();
    if (failures == 0) std::cout << "charter panel model tests passed\n";
    return failures == 0 ? 0 : 1;
}
