// CH1, CH3 -- docs/roadmap/CHARTE-2026-11.md. A charter written back by
// the engine: its canonical text (DumpMotionKit), its edits by bounded
// paths (EditMotionKit), and its versions and file in the personal base
// (rule R, the write order, StaleKit, KitFileDiverged, the restore). Each
// library check works in its own temporary folder, never the user's.
//
// Portable: plain C++ and SQLite.

#include "Json.h"
#include "MotionKitEdit.h"
#include "MotionLibraryBase.h"
#include "MotionTemplates.h"
#include "TestSupport.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;

Value Parsed(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        Check(false, "test JSON does not parse: " + error + ": " + text);
    }
    return value;
}

MotionKitEdit Set(const std::string& path, const std::string& json,
                  bool insert = false) {
    MotionKitEdit edit;
    edit.path = path;
    edit.value = Parsed(json);
    edit.insert = insert;
    return edit;
}

MotionKitEdit Remove(const std::string& path) {
    MotionKitEdit edit;
    edit.path = path;
    return edit;
}

// A charter as a person writes it: keys in their own order, a number with
// a trailing zero, accents and guillemets.
const char kHandWritten[] =
    R"({"format": "brand-kit.v1",)"
    R"( "description": "Charte de démonstration, « marque »",)"
    R"( "props": {"texte": "#FFFFFF", "accent": "#1E4FA0", "police": "Exemple Sans"},)"
    R"( "palette": ["#E0A030", "#101820"],)"
    R"( "captions": {"font_family": "Exemple Sans", "color": "#FFFFFF@0.50",)"
    R"( "formats": {"9x16": {"width": 1080, "height": 1920, "font_size": 64}}},)"
    R"( "pieces": {"logo": {"largeur": 12.50, "coin": "haut-gauche"}},)"
    R"( "sources": ["Planche 2, les couleurs"]})";

// ---- CH1 ----------------------------------------------------------------

void CheckDumpStable() {
    const Value file = Parsed(kHandWritten);
    const std::string once = DumpMotionKit(file);
    const std::string twice = DumpMotionKit(Parsed(once));
    Check(once == twice,
          "the canonical text is idempotent:\n" + once + "---\n" + twice);
    Check(!once.empty() && once.back() == '\n' &&
              once.substr(once.size() - 2) == "}\n",
          "it ends with one newline");
    Check(once.find("\"largeur\": 12.50") != std::string::npos,
          "a number keeps its literal, trailing zero included:\n" + once);
    Check(once.find("Charte de démonstration, « marque »") != std::string::npos,
          "UTF-8 is written as it is, not escaped");
    const size_t format = once.find("\"format\"");
    const size_t description = once.find("\"description\"");
    const size_t props = once.find("\"props\"");
    const size_t texte = once.find("\"texte\"");
    const size_t accent = once.find("\"accent\"");
    const size_t sources = once.find("\"sources\"");
    Check(format < description && description < props && props < texte &&
              texte < accent && accent < sources,
          "the keys keep the file's order, never sorted:\n" + once);
    Check(once.find("\"palette\": [\"#E0A030\", \"#101820\"]") !=
              std::string::npos,
          "a short list of scalars stays on its line:\n" + once);
    Check(once.find("\n  \"props\": {\n    \"texte\": \"#FFFFFF\",") !=
              std::string::npos,
          "two spaces per level, one member per line:\n" + once);
}

void CheckDumpRoundTrip() {
    MotionKitInfo before;
    MotionKitInfo after;
    std::string error;
    Check(ParseMotionKit("marque", kHandWritten, before, error),
          "the hand-written charter reads: " + error);
    Check(ParseMotionKit("marque", DumpMotionKit(Parsed(kHandWritten)), after,
                         error),
          "its canonical text reads: " + error);
    Check(before.name == after.name &&
              before.description == after.description &&
              before.principle == after.principle &&
              before.format == after.format &&
              before.props.Dump() == after.props.Dump() &&
              before.palette == after.palette && before.fonts == after.fonts &&
              before.captions.Dump() == after.captions.Dump() &&
              before.sources == after.sources &&
              before.pieces.Dump() == after.pieces.Dump() &&
              before.withdrawn.Dump() == after.withdrawn.Dump(),
          "the canonical text is the same charter, field by field");
    Check(MotionKitColors(before) == MotionKitColors(after) &&
              MotionKitFonts(before) == MotionKitFonts(after),
          "and allows the same colours and fonts");
}

// One edit on `file`, kept in `out`; the error when refused.
bool EditOne(const Value& file, const MotionKitEdit& edit, Value& out,
             MotionKitError& error, bool* changed = nullptr) {
    bool didChange = false;
    const bool done =
        EditMotionKit("marque", file, {edit}, out, didChange, error);
    if (changed != nullptr) {
        *changed = didChange;
    }
    return done;
}

std::string At(const Value& file, const std::string& path) {
    const Value* found = FindMotionKitPath(file, path);
    return found != nullptr ? found->Dump() : "<none>";
}

void CheckEditPaths() {
    const Value file = Parsed(kHandWritten);
    Value out;
    MotionKitError error;
    const auto refusedAs = [&](const MotionKitEdit& edit, const char* code,
                               const std::string& reason) {
        Value untouched = Parsed(R"({"sentinel":true})");
        bool changed = true;
        const bool refused =
            !EditMotionKit("marque", file, {edit}, untouched, changed, error) &&
            error.code == code &&
            error.message.find(reason) != std::string::npos &&
            untouched.Dump() == R"({"sentinel":true})";
        if (!refused) {
            std::cerr << "  " << edit.path << ": " << error.code << " "
                      << error.message << '\n';
        }
        return refused;
    };

    // Scalars of the top level: replaced in place, a new key last.
    Check(EditOne(file, Set("description", R"("Une autre charte")"), out,
                  error) &&
              out.AsObject()[1].first == "description" &&
              At(out, "description") == R"("Une autre charte")",
          "description is replaced in its rank: " + error.message);
    Check(EditOne(file, Set("principle", R"("Une barre sous le titre.")"), out,
                  error) &&
              out.AsObject().back().first == "principle",
          "a new key goes last: " + error.message);
    // props.<role>.
    Check(EditOne(file, Set("props.accent", R"("#2A6FDB")"), out, error) &&
              At(out, "props.accent") == R"("#2A6FDB")" &&
              out.Find("props")->AsObject()[1].first == "accent",
          "props.accent in place: " + error.message);
    Check(EditOne(file, Set("props.fond", R"("#000000")"), out, error) &&
              out.Find("props")->AsObject().back().first == "fond",
          "props.fond added last: " + error.message);
    Check(EditOne(file, Remove("props.texte"), out, error) &&
              At(out, "props.texte") == "<none>",
          "props.texte removed: " + error.message);
    Check(refusedAs(Set("props.couleur", R"("#000000")"), kMotionKitInvalidKit,
                    "'props.couleur' is not a prop pieces share"),
          "a role the format does not have is ParseMotionKit's to refuse");
    // Lists, whole and by item.
    Check(EditOne(file, Set("palette", R"(["#000000"])"), out, error) &&
              At(out, "palette") == R"(["#000000"])",
          "palette whole: " + error.message);
    Check(EditOne(file, Set("palette.1", R"("#FFFFFF")"), out, error) &&
              At(out, "palette") == R"(["#E0A030","#FFFFFF"])",
          "palette.1 replaced: " + error.message);
    Check(EditOne(file, Set("palette.0", R"("#FFFFFF")", true), out, error) &&
              At(out, "palette") == R"(["#FFFFFF","#E0A030","#101820"])",
          "palette.0 inserted before: " + error.message);
    Check(EditOne(file, Set("palette.2", R"("#FFFFFF")", true), out, error) &&
              At(out, "palette") == R"(["#E0A030","#101820","#FFFFFF"])",
          "an insert at the size appends: " + error.message);
    Check(EditOne(file, Remove("palette.0"), out, error) &&
              At(out, "palette") == R"(["#101820"])",
          "palette.0 removed: " + error.message);
    Check(refusedAs(Set("palette.2", R"("#FFFFFF")"), kMotionKitInvalidEdit,
                    "has 2 item(s): no index 2 to replace"),
          "a set past the end is refused");
    Check(refusedAs(Set("palette.3", R"("#FFFFFF")", true),
                    kMotionKitInvalidEdit, "has 2 item(s): no index 3"),
          "an insert past the size is refused");
    Check(refusedAs(Set("palette.01", R"("#FFFFFF")"), kMotionKitInvalidEdit,
                    "is not a place a charter has"),
          "an index has no leading zero");
    Check(
        EditOne(file, Set("fonts.0", R"("Exemple Serif")", true), out, error) &&
            At(out, "fonts") == R"(["Exemple Serif"])",
        "an insert at 0 creates the list it goes in: " + error.message);
    Check(refusedAs(Set("fonts.0", R"("Exemple Serif")"), kMotionKitInvalidEdit,
                    "there is no 'fonts'"),
          "a set in a list that does not exist is refused");
    Check(EditOne(file, Remove("sources"), out, error) &&
              out.Find("sources") == nullptr,
          "sources removed whole: " + error.message);
    Check(EditOne(file, Remove("palette.7"), out, error) &&
              DumpMotionKit(out) == DumpMotionKit(file),
          "removing an item past the end changes nothing: " + error.message);
    // Captions.
    Check(EditOne(file, Set("captions.color", R"("#E0A030")"), out, error) &&
              At(out, "captions.color") == R"("#E0A030")",
          "captions.color: " + error.message);
    Check(EditOne(file, Set("captions.formats.9x16.font_size", "72"), out,
                  error) &&
              At(out, "captions.formats.9x16") ==
                  R"({"width":1080,"height":1920,"font_size":72})",
          "captions.formats.9x16.font_size: " + error.message);
    Check(EditOne(file,
                  Set("captions.formats.16x9",
                      R"({"width":1920,"height":1080,"font_size":56})"),
                  out, error) &&
              out.Find("captions")->Find("formats")->AsObject().size() == 2,
          "a caption format whole: " + error.message);
    Check(EditOne(file, Set("captions.formats.9x16.height", "1350"), out,
                  error) &&
              At(out, "captions.formats.9x16.height") == "1350",
          "a caption format's height: " + error.message);
    Check(
        EditOne(file, Set("captions", R"({"color":"#000000"})"), out, error) &&
            At(out, "captions") == R"({"color":"#000000"})",
        "the caption style whole: " + error.message);
    Check(refusedAs(Set("captions.formats", "{}"), kMotionKitInvalidEdit,
                    "is not a place a charter has"),
          "captions.formats alone is not a path");
    Check(refusedAs(Set("captions.taille", "40"), kMotionKitInvalidKit,
                    "'captions.taille' is not a caption setting"),
          "a setting the style lacks is refused by ParseMotionKit");
    // Pieces, for every format and for one.
    Check(
        EditOne(file, Set("pieces.logo.coin", R"("bas-droite")"), out, error) &&
            At(out, "pieces.logo") ==
                R"({"largeur":12.50,"coin":"bas-droite"})",
        "pieces.logo.coin in place: " + error.message);
    Check(EditOne(file, Set("pieces.logo.formats.9x16.marge_haut", "20"), out,
                  error) &&
              At(out, "pieces.logo") ==
                  R"({"largeur":12.50,"coin":"haut-gauche",)"
                  R"("formats":{"9x16":{"marge_haut":20}}})",
          "pieces.logo.formats.9x16.marge_haut creates its parents: " +
              error.message);
    Check(EditOne(file, Set("pieces.nom.formats.16x9", R"({"corps":56})"), out,
                  error) &&
              At(out, "pieces.nom") == R"({"formats":{"16x9":{"corps":56}}})",
          "a piece format whole, the piece created: " + error.message);
    Check(EditOne(file, Set("pieces.carton", R"({"corps":120})"), out, error) &&
              At(out, "pieces.carton") == R"({"corps":120})",
          "a piece whole: " + error.message);
    Check(refusedAs(Set("pieces.logo.formats", R"({"9x16":{"a":1}})"),
                    kMotionKitInvalidEdit, "is not a place a charter has"),
          "pieces.<p>.formats alone is not a path");
    Check(refusedAs(Set("pieces.logo.formats.9x16.a.b", "1"),
                    kMotionKitInvalidEdit, "is not a place a charter has"),
          "a path deeper than the grammar is refused");
    Check(refusedAs(Set("pieces.logo.largeur", R"({"x":1})"),
                    kMotionKitInvalidKit,
                    "must be a string, a number or a "
                    "boolean"),
          "an object where a value goes is ParseMotionKit's to refuse");
    // Withdrawn.
    Check(EditOne(file, Set("withdrawn.carton", R"("pas pour cette marque")"),
                  out, error) &&
              At(out, "withdrawn") == R"({"carton":"pas pour cette marque"})",
          "withdrawn.carton: " + error.message);
    // Outside the grammar.
    for (const char* path :
         {"logo", "props", "pieces", "withdrawn", "captions.formats",
          "props.accent.x", "description.x", "", "a..b", "pieces.deux mots",
          "pieces.logo.formats.9x16.marge_haut.x", ".description"}) {
        Check(refusedAs(Set(path, "1"), kMotionKitInvalidEdit, "'"),
              std::string("'") + path + "' is outside the grammar");
    }
    Check(refusedAs(Set("palette", R"(["#000000"])", true),
                    kMotionKitInvalidEdit, "an insert puts a value in a list"),
          "an insert names a list item");
    Check(refusedAs(Set("description", "null"), kMotionKitInvalidEdit,
                    "is given null"),
          "null is not a value: a removal is");
    // A parent that is not an object.
    const Value odd = Parsed(
        R"({"format":"brand-kit.v1","description":"x","captions":"aucune"})");
    Check(!EditOne(odd, Set("captions.color", R"("#000000")"), out, error) &&
              error.code == kMotionKitInvalidEdit &&
              error.message.find("'captions' is not an object") !=
                  std::string::npos,
          "a value cannot be written inside a scalar: " + error.message);
    Check(!EditOne(Parsed(R"(["x"])"), Set("description", R"("x")"), out,
                   error) &&
              error.code == kMotionKitInvalidKit,
          "a tree that is not an object is no charter: " + error.message);

    // Pruning: what a removal leaves empty goes, up to the top level.
    const Value lone =
        Parsed(R"({"format":"brand-kit.v1","description":"Marque.",)"
               R"("props":{"accent":"#1E4FA0"},)"
               R"("pieces":{"logo":{"formats":{"9x16":{"marge_haut":20}}}}})");
    Check(EditOne(lone, Remove("pieces.logo.formats.9x16.marge_haut"), out,
                  error) &&
              out.Find("pieces") == nullptr && out.AsObject().size() == 3,
          "the last per-format value removed takes its format, formats, the "
          "piece and pieces with it: " +
              error.message + " " + out.Dump());
    const Value twoFormats =
        Parsed(R"({"format":"brand-kit.v1","description":"Marque.",)"
               R"("pieces":{"logo":{"coin":"bas-gauche","formats":)"
               R"({"9x16":{"marge_haut":20},"16x9":{"marge_haut":8}}}}})");
    Check(
        EditOne(twoFormats, Remove("pieces.logo.formats.9x16.marge_haut"), out,
                error) &&
            At(out, "pieces.logo") ==
                R"({"coin":"bas-gauche","formats":{"16x9":{"marge_haut":8}}})",
        "pruning stops at the first container something is left in: " +
            error.message);
    Check(EditOne(
              Parsed(R"({"format":"brand-kit.v1","description":"x",)"
                     R"("props":{"accent":"#000000"},"palette":["#000000"]})"),
              Remove("palette.0"), out, error) &&
              out.Find("palette") == nullptr,
          "a list left empty goes too: " + error.message);
    Check(
        !EditOne(Parsed(R"({"format":"brand-kit.v1","description":"x",)"
                        R"("palette":["#000000"]})"),
                 Remove("palette.0"), out, error) &&
            error.code == kMotionKitInvalidKit && error.path == "palette.0" &&
            error.message.find("a charter sets something") != std::string::npos,
        "a charter emptied by a removal is refused by ParseMotionKit, with "
        "the edit's path: " +
            error.message);
}

void CheckEditAtomic() {
    const Value file = Parsed(kHandWritten);
    Value out = Parsed(R"({"sentinel":true})");
    bool changed = true;
    MotionKitError error;
    Check(!EditMotionKit("marque", file,
                         {Set("props.accent", R"("#000000")"),
                          Set("palette.9", R"("#000000")")},
                         out, changed, error) &&
              error.code == kMotionKitInvalidEdit &&
              error.path == "palette.9" && out.Dump() == R"({"sentinel":true})",
          "one edit that cannot be made, none made: " + error.message);
    Check(!EditMotionKit("marque", file,
                         {Set("props.accent", R"("#000000")"),
                          Set("props.texte", R"("bleu roi")")},
                         out, changed, error) &&
              error.code == kMotionKitInvalidKit && error.path.empty() &&
              error.message.find("'props.texte' must be a color") !=
                  std::string::npos &&
              out.Dump() == R"({"sentinel":true})",
          "a batch whose result does not read: InvalidKit, no path: " +
              error.message);
    Check(
        !EditMotionKit("marque", file, {Set("props.texte", R"("bleu roi")")},
                       out, changed, error) &&
            error.code == kMotionKitInvalidKit && error.path == "props.texte",
        "one edit refused by ParseMotionKit names its path: " + error.message);
    // Edits apply in order: the second sees the first.
    Check(EditMotionKit(
              "marque", file,
              {Set("palette.0", R"("#FFFFFF")", true), Remove("palette.1")},
              out, changed, error) &&
              At(out, "palette") == R"(["#FFFFFF","#101820"])" && changed,
          "edits apply in order: " + error.message);
}

void CheckEditNoop() {
    const Value file = Parsed(kHandWritten);
    Value out;
    MotionKitError error;
    bool changed = true;
    Check(EditOne(file, Set("props.accent", R"("#1E4FA0")"), out, error,
                  &changed) &&
              !changed,
          "the same value again changes nothing");
    Check(EditOne(file, Remove("withdrawn.carton"), out, error, &changed) &&
              !changed && DumpMotionKit(out) == DumpMotionKit(file),
          "removing what is not there changes nothing");
    Check(EditOne(file, Remove("pieces.nom.formats.9x16.corps"), out, error,
                  &changed) &&
              !changed,
          "nor removing under a piece that is not there");
    Check(EditOne(file, Set("format", R"("brand-kit.v1")"), out, error,
                  &changed) &&
              !changed,
          "a charter already brand-kit.v1 stays as it is");
    Check(EditOne(file, Set("pieces.logo.largeur", "12.5"), out, error,
                  &changed) &&
              changed && At(out, "pieces.logo.largeur") == "12.5",
          "12.5 is another literal than 12.50: the file says what was typed");
    Check(EditMotionKit("marque", file, {}, out, changed, error) && !changed,
          "no edit, no change: " + error.message);
}

void CheckFormatUpgrade() {
    const Value motionKit =
        Parsed(R"({"format":"motion-kit.v1","description":"Bleu.",)"
               R"("props":{"accent":"#1E4FA0"}})");
    Value out;
    MotionKitError error;
    bool changed = false;
    Check(!EditMotionKit("bleu", motionKit,
                         {Set("palette.0", R"("#E0A030")", true)}, out, changed,
                         error) &&
              error.code == kMotionKitInvalidEdit &&
              error.message.find("set \"format\" to \"brand-kit.v1\"") !=
                  std::string::npos,
          "a charter field on a motion-kit.v1 names the format to write: " +
              error.message);
    Check(!EditMotionKit("bleu", motionKit, {Remove("pieces.logo")}, out,
                         changed, error) &&
              error.code == kMotionKitInvalidEdit,
          "a removal there too: " + error.message);
    Check(EditMotionKit("bleu", motionKit,
                        {Set("palette.0", R"("#E0A030")", true),
                         Set("format", R"("brand-kit.v1")")},
                        out, changed, error) &&
              changed && out.AsObject()[0].first == "format" &&
              At(out, "format") == R"("brand-kit.v1")" &&
              At(out, "palette") == R"(["#E0A030"])",
          "with the format in the same batch, wherever it is, the charter "
          "grows; format keeps its rank: " +
              error.message);
    Check(EditOne(motionKit, Set("props.texte", R"("#FFFFFF")"), out, error) &&
              At(out, "format") == R"("motion-kit.v1")",
          "a motion-kit.v1 edits its props without changing format: " +
              error.message);
    Check(!EditOne(Parsed(kHandWritten), Set("format", R"("motion-kit.v1")"),
                   out, error) &&
              error.code == kMotionKitInvalidEdit &&
              error.message.find("only goes to") != std::string::npos,
          "format never goes back: " + error.message);
    Check(!EditOne(Parsed(kHandWritten), Remove("format"), out, error) &&
              error.code == kMotionKitInvalidEdit,
          "format is never removed: " + error.message);
}

void CheckKitNumber() {
    Check(MotionKitNumber(12.3, 0.5).Dump() == "12.5", "12.3 at 0.5 is 12.5");
    Check(MotionKitNumber(12.0, 0.5).Dump() == "12", "an integer has no point");
    Check(MotionKitNumber(0.0, 0.1).Dump() == "0", "zero is 0");
    Check(MotionKitNumber(-0.0001, 0.1).Dump() == "0", "never -0");
    Check(MotionKitNumber(12.3, 0.1).Dump() == "12.3",
          "12.3 at 0.1 is 12.3, not 12.300000000000001");
    Check(MotionKitNumber(7.25, 0.0).Dump() == "7.25", "no step, no rounding");
    Check(MotionKitNumber(1.0 / 3.0, 0.0).Dump() == "0.333",
          "rounded to a thousandth");
    Check(
        MotionKitNumber(std::numeric_limits<double>::quiet_NaN(), 1.0).Dump() ==
            "0",
        "a value that is not finite is 0");
    Value out;
    MotionKitError error;
    Check(EditOne(Parsed(kHandWritten),
                  {"pieces.logo.formats.9x16.marge_haut",
                   MotionKitNumber(14.26, 0.5), false},
                  out, error) &&
              DumpMotionKit(out).find("\"marge_haut\": 14.5") !=
                  std::string::npos,
          "a dragged value lands in the file as typed: " + error.message);
}

void CheckParseEdits() {
    std::vector<MotionKitEdit> edits;
    MotionKitError error;
    Check(ParseMotionKitEdits(
              Parsed(R"([{"path":"props.accent","value":"#000000"},)"
                     R"({"path":"palette.0","value":"#FFFFFF","insert":true},)"
                     R"({"path":"withdrawn.carton","remove":true},)"
                     R"({"path":"description","value":"x","insert":false}])"),
              edits, error) &&
              edits.size() == 4 && edits[0].value.has_value() &&
              edits[0].value->AsString() == "#000000" && !edits[0].insert &&
              edits[1].insert && !edits[2].value.has_value() &&
              edits[2].path == "withdrawn.carton" && !edits[3].insert,
          "sets, an insert and a removal: " + error.message);
    const auto refused = [&](const std::string& json, const std::string& path,
                             const std::string& reason) {
        std::vector<MotionKitEdit> got;
        const bool done =
            !ParseMotionKitEdits(Parsed(json), got, error) &&
            error.code == kMotionKitInvalidEdit && error.path == path &&
            error.message.find(reason) != std::string::npos && got.empty();
        if (!done) {
            std::cerr << "  " << json << ": " << error.path << " "
                      << error.message << '\n';
        }
        return done;
    };
    Check(refused(R"({"path":"x","value":1})", "edits", "non-empty array"),
          "an object is not a list of edits");
    Check(refused("[]", "edits", "non-empty array"), "an empty list");
    Check(refused(R"([{"path":"a","value":1},"x"])", "edits[1]",
                  "must be an object"),
          "an item that is not an object, named by its index");
    Check(refused(R"([{"path":"a","value":1,"op":"set"}])", "edits[0]",
                  "unknown key 'op'"),
          "a key the format does not know");
    Check(refused(R"([{"value":1}])", "edits[0]", ".path' must be"), "no path");
    Check(refused(R"([{"path":"","value":1}])", "edits[0]", ".path' must be"),
          "an empty path");
    Check(refused(R"([{"path":"a","value":null}])", "edits[0]",
                  "is null: write \"remove\": true"),
          "null is refused");
    Check(refused(R"([{"path":"a","value":1,"remove":true}])", "edits[0]",
                  "not both"),
          "a set and a removal at once");
    Check(refused(R"([{"path":"a"}])", "edits[0]", "not neither"),
          "neither a set nor a removal");
    Check(
        refused(R"([{"path":"a","remove":false}])", "edits[0]", "not neither"),
        "remove: false is no removal");
    Check(refused(R"([{"path":"a","remove":"oui"}])", "edits[0]",
                  ".remove' must be true or false"),
          "remove is a boolean");
    Check(refused(R"([{"path":"a.0","remove":true,"insert":true}])", "edits[0]",
                  "an insert takes a value"),
          "an insert takes a value");
    Check(refused(R"([{"path":"a.0","value":1,"insert":1}])", "edits[0]",
                  ".insert' must be true or false"),
          "insert is a boolean");
}

// ---- CH3 ----------------------------------------------------------------

// A library of its own, removed at the end of the check.
struct TempLibrary {
    explicit TempLibrary(const std::string& tag)
        : root(fs::temp_directory_path() / ("cutmachine-kit-edit-" + tag + "-" +
                                            std::to_string(::getpid()))) {
        fs::remove_all(root);
        fs::create_directories(root);
        location = MotionLibraryLocation{(root / "library.sqlite").string(),
                                         root.string()};
    }
    ~TempLibrary() {
        std::error_code ignored;
        fs::permissions(root / "kits", fs::perms::owner_all,
                        fs::perm_options::add, ignored);
        fs::remove_all(root, ignored);
    }
    TempLibrary(const TempLibrary&) = delete;
    TempLibrary& operator=(const TempLibrary&) = delete;

    fs::path Kit(const std::string& name) const {
        return root / "kits" / (name + ".json");
    }

    fs::path root;
    MotionLibraryLocation location;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

int64_t KitVersions(const std::string& base, const std::string& name) {
    sqlite3* db = nullptr;
    sqlite3_open_v2(base.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT count(*) FROM motion_library WHERE kind = 'kit' "
                       "AND name = ?1",
                       -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    const int64_t count = sqlite3_step(statement) == SQLITE_ROW
                              ? sqlite3_column_int64(statement, 0)
                              : -1;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return count;
}

// A version written behind the engine's back: what an older or a later
// binary may leave, which this one does not read.
bool InsertRow(const std::string& base, const std::string& name,
               int64_t version, const std::string& content) {
    sqlite3* db = nullptr;
    sqlite3_open_v2(base.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db,
                       "INSERT INTO motion_library (kind, name, version, "
                       "content_json, content_hash, source) VALUES ('kit', "
                       "?1, ?2, ?3, 'test', 'test')",
                       -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, version);
    sqlite3_bind_text(statement, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    const bool done = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return done;
}

// No temporary file left in kits/ (they all start with a dot).
bool NoTemporary(const TempLibrary& library) {
    std::error_code failure;
    for (fs::directory_iterator entry(library.root / "kits", failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const std::string name = entry->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            std::cerr << "  left behind: " << name << '\n';
            return false;
        }
    }
    return true;
}

const char kMarque[] =
    R"({"format":"brand-kit.v1","description":"Charte de la marque.",)"
    R"("props":{"accent":"#1E4FA0","police":"Exemple Sans"},)"
    R"("pieces":{"logo":{"marge_haut":10}}})";

void CheckKitSave() {
    TempLibrary library("save");
    const MotionLibraryLocation& location = library.location;
    const fs::path file = library.Kit("marque");
    MotionKitSaved saved;
    MotionKitError error;

    // Refused before the base: a name that is not a kit's, a tree that is
    // not a charter. Nothing is created.
    Check(!SaveMotionKitVersion(location, "../marque", Parsed(kMarque), 0,
                                "panel", saved, error) &&
              error.code == kMotionKitInvalidKit,
          "a name that is a path is refused: " + error.message);
    Check(!SaveMotionKitVersion(location, "marque",
                                Parsed(R"({"format":"brand-kit.v1"})"), 0,
                                "panel", saved, error) &&
              error.code == kMotionKitInvalidKit &&
              error.message.find("'description' is required") !=
                  std::string::npos &&
              !fs::exists(location.base) && !fs::exists(file),
          "a tree that is not a charter is refused, nothing written: " +
              error.message);

    // Created: v1, the file's bytes are the version's, read without import.
    Check(
        SaveMotionKitVersion(location, "marque", Parsed(kMarque), 0, "panel",
                             saved, error) &&
            saved.created && saved.version == 1 &&
            saved.path == file.string() && ReadFile(file) == saved.content &&
            saved.content == DumpMotionKit(Parsed(kMarque)),
        "created as v1, the file written in canonical form: " + error.message);
    std::vector<MotionKitVersion> versions;
    Check(ListMotionKitVersions(location, "marque", 0, versions, error) &&
              versions.size() == 1 && versions[0].content == ReadFile(file) &&
              versions[0].source == "panel" && versions[0].issue.empty() &&
              !versions[0].created_at.empty(),
          "the stored bytes are the file's: " + error.message);
    MotionKitState state;
    Check(ReadMotionKitState(location, "marque", state, error) &&
              state.file == MotionKitFileState::Matches &&
              state.imported.empty() && state.latest.version == 1 &&
              state.kit.version == 1 &&
              state.kit.description == "Charte de la marque.",
          "the next read finds the file equal, imports nothing: " +
              error.message);
    std::vector<MotionTemplateInfo> shipped;
    std::string reason;
    Check(MotionTemplateCatalog(shipped, reason), "catalog: " + reason);
    MotionLibraryContents contents;
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.imported.empty() && contents.issues.empty() &&
              contents.kits.size() == 1 && contents.kits[0].version == 1,
          "nor does the shelf");

    // Stale: written over a version that is not the latest, nothing moves.
    const std::string v1 = ReadFile(file);
    Check(!SaveMotionKitVersion(location, "marque", Parsed(kHandWritten), 0,
                                "panel", saved, error) &&
              error.code == kMotionKitStaleKit && saved.version == 1 &&
              ReadFile(file) == v1 && KitVersions(location.base, "marque") == 1,
          "creating a kit that exists is stale: " + error.message);
    Check(!SaveMotionKitVersion(location, "marque", Parsed(kHandWritten), 4,
                                "panel", saved, error) &&
              error.code == kMotionKitStaleKit && saved.version == 1 &&
              ReadFile(file) == v1,
          "a version from elsewhere is stale: " + error.message);

    // The same charter, written another way: no version.
    Check(SaveMotionKitVersion(location, "marque",
                               Parsed(std::string("\n\n  ") + kMarque + "  "),
                               1, "panel", saved, error) &&
              !saved.created && saved.version == 1 &&
              KitVersions(location.base, "marque") == 1 && ReadFile(file) == v1,
          "a reformatted charter is not a new version: " + error.message);

    // An edit: one version, the file follows, no import after it.
    Check(
        EditMotionKitVersion(location, "marque",
                             {Set("pieces.logo.formats.9x16.marge_haut", "20")},
                             1, "edit_motion_kit", saved, error) &&
            saved.created && saved.version == 2 &&
            ReadFile(file) == saved.content &&
            saved.content.find("\"marge_haut\": 20") != std::string::npos,
        "an edit is the next version, in the file: " + error.message);
    Check(ReadMotionKitState(location, "marque", state, error) &&
              state.imported.empty() && state.latest.version == 2 &&
              state.latest.source == "edit_motion_kit",
          "and is not imported again: " + error.message);
    Check(EditMotionKitVersion(location, "marque",
                               {Set("pieces.logo.marge_haut", "10")}, 2,
                               "panel", saved, error) &&
              !saved.created && saved.version == 2,
          "an edit that changes nothing writes no version: " + error.message);
    Check(!EditMotionKitVersion(location, "marque",
                                {Set("props.accent", R"("bleu roi")")}, 2,
                                "panel", saved, error) &&
              error.code == kMotionKitInvalidKit &&
              error.path == "props.accent" &&
              KitVersions(location.base, "marque") == 2,
          "an edit the charter refuses writes nothing: " + error.message);
    Check(!EditMotionKitVersion(location, "marque",
                                {Set("props.accent", R"("#000000")"),
                                 Set("palette.4", R"("#000000")")},
                                2, "panel", saved, error) &&
              error.code == kMotionKitInvalidEdit &&
              KitVersions(location.base, "marque") == 2 &&
              ReadFile(file).find("#000000") == std::string::npos,
          "one bad edit of a batch: the file and the base unchanged: " +
              error.message);

    // A hand edit is imported; a write over the version before it is stale.
    const std::string hand =
        R"({"format":"brand-kit.v1","description":"Retouchée à la main.",)"
        R"("props":{"accent":"#1E4FA0","police":"Exemple Sans"}})";
    WriteFile(file, hand);
    Check(!SaveMotionKitVersion(location, "marque", Parsed(kMarque), 2, "panel",
                                saved, error) &&
              error.code == kMotionKitStaleKit && saved.version == 3 &&
              saved.imported.size() == 1 &&
              saved.imported[0].message == "kit marque v3" &&
              KitVersions(location.base, "marque") == 3 &&
              ReadFile(file) == hand,
          "the hand edit enters as v3 under the write's lock, and the write "
          "over v2 is stale: " +
              error.message);
    Check(ReadMotionKitState(location, "marque", state, error) &&
              state.latest.version == 3 && state.latest.content == hand &&
              state.latest.source == file.string() && state.imported.empty(),
          "the import is kept, with the file as its source: " + error.message);

    // A hand revert to the bytes of v1 is a new version, not ignored.
    WriteFile(file, v1);
    Check(ReadMotionKitState(location, "marque", state, error) &&
              state.imported.size() == 1 && state.latest.version == 4 &&
              state.latest.content == v1,
          "back to older bytes by hand is v4: " + error.message);
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.imported.empty() && contents.kits.size() == 1 &&
              contents.kits[0].version == 4,
          "the shelf agrees");

    // A file that no longer reads: every write refused, the file untouched,
    // the latest version still listed.
    WriteFile(file, "{\"format\": \"brand-kit.v1\",");
    Check(!SaveMotionKitVersion(location, "marque", Parsed(kMarque), 4, "panel",
                                saved, error) &&
              error.code == kMotionKitFileDiverged &&
              error.path == file.string() &&
              error.message.find("not JSON") != std::string::npos,
          "a save over a broken file: KitFileDiverged: " + error.message);
    Check(!EditMotionKitVersion(location, "marque",
                                {Set("description", R"("x")")}, 4, "panel",
                                saved, error) &&
              error.code == kMotionKitFileDiverged,
          "an edit too: " + error.message);
    Check(!RestoreMotionKitVersion(location, "marque", 2, 4, saved, error) &&
              error.code == kMotionKitFileDiverged,
          "a restore too: " + error.message);
    Check(ReadFile(file) == "{\"format\": \"brand-kit.v1\"," &&
              KitVersions(location.base, "marque") == 4,
          "the broken file is left as it is, nothing is versioned");
    Check(
        ReadMotionKitState(location, "marque", state, error) &&
            state.file == MotionKitFileState::Diverged &&
            state.file_issue.find("not JSON") != std::string::npos &&
            state.latest.version == 4 && state.kit.version == 4,
        "the state says the file diverged and still has v4: " + error.message);
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.kits.size() == 1 && contents.kits[0].version == 4 &&
              contents.issues.size() == 1 &&
              contents.issues[0].path == file.string(),
          "the shelf lists v4 and names the file as an issue");
    MotionKitInfo found;
    Check(FindMotionLibraryKit(location, "marque", found, reason) &&
              found.version == 4,
          "a tool taking the kit by name still gets v4: " + reason);

    // The file removed: recreated by a write that adds nothing.
    fs::remove(file);
    Check(ReadMotionKitState(location, "marque", state, error) &&
              state.file == MotionKitFileState::Absent &&
              state.latest.version == 4,
          "an absent file leaves the base as it is: " + error.message);
    Check(SaveMotionKitVersion(location, "marque", Parsed(v1), 4, "panel",
                               saved, error) &&
              !saved.created && saved.version == 4 && ReadFile(file) == v1,
          "a write equal to the latest recreates the absent file with its "
          "bytes: " +
              error.message);

    // Restore: an older version as a new one; restoring it again adds none.
    Check(RestoreMotionKitVersion(location, "marque", 3, 4, saved, error) &&
              saved.created && saved.version == 5 &&
              ReadFile(file) == saved.content &&
              saved.content == DumpMotionKit(Parsed(hand)),
          "restore v3 is v5, in canonical form: " + error.message);
    Check(ListMotionKitVersions(location, "marque", 2, versions, error) &&
              versions.size() == 2 && versions[0].version == 5 &&
              versions[0].source == "restore v3" && versions[1].version == 4,
          "newest first, limited, the restore named: " + error.message);
    Check(
        RestoreMotionKitVersion(location, "marque", 3, 5, saved, error) &&
            !saved.created && saved.version == 5,
        "restoring what is the latest already adds nothing: " + error.message);
    Check(!RestoreMotionKitVersion(location, "marque", 9, 5, saved, error) &&
              error.code == kMotionKitUnknownKit,
          "a version the base does not hold: " + error.message);
    Check(!RestoreMotionKitVersion(location, "marque", 1, 3, saved, error) &&
              error.code == kMotionKitStaleKit && saved.version == 5,
          "a restore over an old version is stale: " + error.message);
    Check(!EditMotionKitVersion(location, "absente",
                                {Set("description", R"("x")")}, 0, "panel",
                                saved, error) &&
              error.code == kMotionKitUnknownKit,
          "an edit of a kit that does not exist: " + error.message);
    Check(!ReadMotionKitState(location, "absente", state, error) &&
              error.code == kMotionKitUnknownKit,
          "nor can it be read: " + error.message);
    Check(!ListMotionKitVersions(location, "absente", 0, versions, error) &&
              error.code == kMotionKitUnknownKit,
          "nor listed: " + error.message);
    Check(NoTemporary(library), "no temporary file is left in kits/");
}

// A latest version this binary does not read: listed, its file not taken
// for a diverged one, and an older version restored over it.
void CheckBrokenLatest() {
    TempLibrary library("broken");
    const MotionLibraryLocation& location = library.location;
    MotionKitSaved saved;
    MotionKitError error;
    Check(SaveMotionKitVersion(location, "bleu", Parsed(kMarque), 0, "panel",
                               saved, error),
          "v1: " + error.message);
    const std::string unread =
        R"({"format":"brand-kit.v1","description":"Plus tard.",)"
        R"("props":{"accent":"#1E4FA0"},"motion":{"vitesse":2}})";
    Check(InsertRow(location.base, "bleu", 2, unread), "a v2 from elsewhere");
    WriteFile(library.Kit("bleu"), unread);
    MotionKitState state;
    Check(ReadMotionKitState(location, "bleu", state, error) &&
              state.file == MotionKitFileState::Matches &&
              state.latest.version == 2 &&
              state.latest.issue.find("unknown field 'motion'") !=
                  std::string::npos &&
              state.kit.name == "bleu" && state.kit.version == 0 &&
              state.kit.description.empty() && state.imported.empty(),
          "a file equal to a latest that does not read is not diverged: " +
              error.message);
    std::vector<MotionKitVersion> versions;
    Check(ListMotionKitVersions(location, "bleu", 0, versions, error) &&
              versions.size() == 2 && !versions[0].issue.empty() &&
              versions[1].issue.empty(),
          "its versions are listed, the issue on the one that does not "
          "read: " +
              error.message);
    std::vector<std::string> names;
    Check(ListMotionKitNames(location, names, error) &&
              names == std::vector<std::string>{"bleu"},
          "its name is listed: " + error.message);
    Check(!EditMotionKitVersion(location, "bleu", {Remove("motion")}, 2,
                                "panel", saved, error) &&
              error.code == kMotionKitInvalidEdit,
          "an edit outside the grammar cannot mend it: " + error.message);
    Check(RestoreMotionKitVersion(location, "bleu", 1, 2, saved, error) &&
              saved.created && saved.version == 3 &&
              ReadFile(library.Kit("bleu")) == saved.content &&
              saved.content == DumpMotionKit(Parsed(kMarque)),
          "the older version restored over it: " + error.message);
    Check(ReadMotionKitState(location, "bleu", state, error) &&
              state.latest.issue.empty() && state.kit.version == 3,
          "and the charter reads again: " + error.message);
}

// The names the panel lists, broken ones included; a link and a pipe are
// diverged, never followed; a library without an inbox writes the base.
void CheckKitNamesAndFiles() {
    TempLibrary library("names");
    const MotionLibraryLocation& location = library.location;
    std::vector<std::string> names;
    MotionKitError error;
    Check(
        ListMotionKitNames(location, names, error) && names.empty() &&
            !fs::exists(location.base),
        "an empty library lists nothing and leaves no base: " + error.message);
    WriteFile(library.Kit("cassee"), "{");
    WriteFile(library.Kit("rose"),
              R"({"format":"motion-kit.v1","description":"Rose.",)"
              R"("props":{"accent":"#E0A030"}})");
    WriteFile(library.Kit("mal nomme"), "{}");
    WriteFile(library.root / "kits" / ".cachee.json", "{}");
    WriteFile(library.root / "elsewhere" / "vert.json",
              R"({"format":"motion-kit.v1","description":"Vert.",)"
              R"("props":{"accent":"#0A7C66"}})");
    fs::create_symlink(library.root / "elsewhere" / "vert.json",
                       library.Kit("lien"));
    Check(ListMotionKitNames(location, names, error) &&
              names == std::vector<std::string>{"cassee", "lien", "rose"},
          "the names: files that read, a broken one and a link, sorted, "
          "not a hidden file nor a name that is not one");
    MotionKitState state;
    Check(ReadMotionKitState(location, "cassee", state, error) &&
              state.latest.version == 0 &&
              state.file == MotionKitFileState::Diverged &&
              state.file_issue.find("not JSON") != std::string::npos,
          "a broken file without any version still has a state: " +
              error.message);
    std::vector<MotionKitVersion> versions;
    Check(!ListMotionKitVersions(location, "cassee", 0, versions, error) &&
              error.code == kMotionKitUnknownKit,
          "but no version: " + error.message);
    Check(ReadMotionKitState(location, "lien", state, error) &&
              state.file == MotionKitFileState::Diverged &&
              state.file_issue.find("symbolic link") != std::string::npos &&
              state.latest.version == 0,
          "a link is diverged, never followed: " + error.message);
    MotionKitSaved saved;
    Check(!SaveMotionKitVersion(location, "lien",
                                Parsed(R"({"format":"motion-kit.v1",)"
                                       R"("description":"x",)"
                                       R"("props":{"accent":"#000000"}})"),
                                0, "panel", saved, error) &&
              error.code == kMotionKitFileDiverged &&
              fs::is_symlink(library.Kit("lien")),
          "and not written over: " + error.message);
    Check(::mkfifo(library.Kit("tube").c_str(), 0600) == 0, "a pipe");
    Check(
        ReadMotionKitState(location, "tube", state, error) &&
            state.file == MotionKitFileState::Diverged &&
            state.file_issue == "not a regular file",
        "a pipe is diverged, and reading it does not block: " + error.message);
    Check(ReadMotionKitState(location, "rose", state, error) &&
              state.kit.format == kMotionKitFormat && state.kit.version == 1,
          "a motion-kit.v1 file reads as v1: " + error.message);

    // Without an inbox: the base alone.
    const MotionLibraryLocation baseOnly{
        (library.root / "seule.sqlite").string(), ""};
    Check(SaveMotionKitVersion(baseOnly, "marque", Parsed(kMarque), 0, "panel",
                               saved, error) &&
              saved.created && saved.version == 1 && saved.path.empty(),
          "without an inbox the base alone is written: " + error.message);
    Check(ReadMotionKitState(baseOnly, "marque", state, error) &&
              state.path.empty() && state.file == MotionKitFileState::Absent &&
              state.kit.version == 1,
          "and read: " + error.message);
    Check(ListMotionKitNames(baseOnly, names, error) &&
              names == std::vector<std::string>{"marque"},
          "and listed: " + error.message);
    Check(!ReadMotionKitState(location, "../marque", state, error) &&
              error.code == kMotionKitUnknownKit,
          "a name that is a path names no kit: " + error.message);
}

// A write that cannot reach the file rolls back and leaves nothing behind.
void CheckKitWriteFailure() {
    if (::geteuid() == 0) {
        return;  // Permissions do not stop root.
    }
    TempLibrary library("failure");
    const MotionLibraryLocation& location = library.location;
    MotionKitSaved saved;
    MotionKitError error;
    Check(SaveMotionKitVersion(location, "marque", Parsed(kMarque), 0, "panel",
                               saved, error),
          "v1: " + error.message);
    const std::string v1 = ReadFile(library.Kit("marque"));
    fs::permissions(library.root / "kits",
                    fs::perms::owner_read | fs::perms::owner_exec);
    Check(
        !EditMotionKitVersion(location, "marque",
                              {Set("description", R"("Ailleurs.")")}, 1,
                              "panel", saved, error) &&
            error.code == kMotionKitIoError &&
            KitVersions(location.base, "marque") == 1,
        "a file that cannot be written: IoError, no version: " + error.message);
    fs::permissions(library.root / "kits", fs::perms::owner_all);
    Check(ReadFile(library.Kit("marque")) == v1 && NoTemporary(library),
          "the file as it was, no temporary file");
}

// The entries of kits/ as the folder spells them, sorted: what a disk that
// ignores case holds, which fs::exists cannot tell (it answers yes to
// kits/Marque.json when kits/marque.json is there).
std::vector<std::string> KitEntries(const TempLibrary& library) {
    std::vector<std::string> entries;
    std::error_code failure;
    for (fs::directory_iterator entry(library.root / "kits", failure), end;
         !failure && entry != end; entry.increment(failure)) {
        entries.push_back(entry->path().filename().string());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

// CH3 (review) -- two kit names that differ only by case are one file on a
// disk that ignores case, macOS's by default: kits/Marque.json opens
// kits/marque.json. Rule R reads a file only under its exact name, and a
// writer refuses a name that folds to another kit's. Written to pass the
// same on a disk that tells case apart: the refusals are the rule there
// too, and nothing reads kits/Marque.json but the exact entry.
void CheckKitNameCase() {
    {
        TempLibrary library("case");
        const MotionLibraryLocation& location = library.location;
        MotionKitSaved saved;
        MotionKitError error;
        Check(SaveMotionKitVersion(location, "marque", Parsed(kMarque), 0,
                                   "panel", saved, error),
              "marque v1: " + error.message);
        const std::string v1 = ReadFile(library.Kit("marque"));
        Check(!SaveMotionKitVersion(location, "Marque", Parsed(kHandWritten), 0,
                                    "panel", saved, error) &&
                  error.code == kMotionKitInvalidKit &&
                  error.message.find("'marque'") != std::string::npos &&
                  error.message.find("case") != std::string::npos,
              "saving Marque beside marque is refused: " + error.message);
        Check(KitEntries(library) == std::vector<std::string>{"marque.json"} &&
                  ReadFile(library.Kit("marque")) == v1 &&
                  KitVersions(location.base, "Marque") == 0 &&
                  KitVersions(location.base, "marque") == 1,
              "neither the file of marque nor the base moved");
        // A version of Marque left by an older binary: no writer touches it
        // while marque is there, edits and restores included.
        Check(InsertRow(location.base, "Marque", 1, kHandWritten),
              "a Marque v1 from elsewhere");
        Check(!EditMotionKitVersion(location, "Marque",
                                    {Set("description", R"("Ailleurs.")")}, 1,
                                    "panel", saved, error) &&
                  error.code == kMotionKitInvalidKit,
              "nor edited: " + error.message);
        Check(
            !RestoreMotionKitVersion(location, "Marque", 1, 1, saved, error) &&
                error.code == kMotionKitInvalidKit,
            "nor restored: " + error.message);
        Check(
            !EditMotionKitVersion(location, "marque",
                                  {Set("description", R"("Ailleurs.")")}, 1,
                                  "panel", saved, error) &&
                error.code == kMotionKitInvalidKit &&
                ReadFile(library.Kit("marque")) == v1 &&
                KitEntries(library) == std::vector<std::string>{"marque.json"},
            "and marque itself waits until one of them is renamed: " +
                error.message);
        MotionKitState state;
        Check(ReadMotionKitState(location, "Marque", state, error) &&
                  state.file == MotionKitFileState::Absent &&
                  state.imported.empty() && state.latest.version == 1 &&
                  state.latest.content == kHandWritten,
              "reading Marque finds no file of its own, and takes nothing "
              "from marque's: " +
                  error.message);
    }
    {
        // Only kits/marque.json, written by hand.
        TempLibrary library("case-file");
        const MotionLibraryLocation& location = library.location;
        WriteFile(library.Kit("marque"), kMarque);
        MotionKitState state;
        MotionKitError error;
        Check(!ReadMotionKitState(location, "Marque", state, error) &&
                  error.code == kMotionKitUnknownKit &&
                  !fs::exists(location.base),
              "Marque has no file: unknown, nothing imported, no base: " +
                  error.message);
        std::vector<MotionKitVersion> versions;
        Check(!ListMotionKitVersions(location, "Marque", 0, versions, error) &&
                  error.code == kMotionKitUnknownKit &&
                  !fs::exists(location.base),
              "nor any version: " + error.message);
        Check(ReadMotionKitState(location, "marque", state, error) &&
                  state.file == MotionKitFileState::Matches &&
                  state.imported.size() == 1 && state.latest.version == 1,
              "marque reads its own file: " + error.message);
        MotionKitInfo kit;
        std::string reason;
        Check(!FindMotionLibraryKit(location, "Marque", kit, reason) &&
                  KitVersions(location.base, "Marque") == 0,
              "a tool asking for Marque does not get marque: " + reason);
        MotionKitSaved saved;
        Check(
            !SaveMotionKitVersion(location, "Marque", Parsed(kHandWritten), 0,
                                  "panel", saved, error) &&
                error.code == kMotionKitInvalidKit &&
                ReadFile(library.Kit("marque")) == kMarque &&
                KitEntries(library) == std::vector<std::string>{"marque.json"},
            "and Marque cannot be written over it: " + error.message);
    }
    {
        // Only kits/Bleu.json: a name the base never held, refused by the
        // folder alone.
        TempLibrary library("case-entry");
        const MotionLibraryLocation& location = library.location;
        WriteFile(library.root / "kits" / "Bleu.json", kMarque);
        MotionKitSaved saved;
        MotionKitError error;
        Check(
            !SaveMotionKitVersion(location, "bleu", Parsed(kHandWritten), 0,
                                  "panel", saved, error) &&
                error.code == kMotionKitInvalidKit &&
                error.message.find("Bleu.json") != std::string::npos &&
                KitEntries(library) == std::vector<std::string>{"Bleu.json"} &&
                ReadFile(library.root / "kits" / "Bleu.json") == kMarque &&
                KitVersions(location.base, "bleu") == 0,
            "bleu beside kits/Bleu.json is refused, the file untouched: " +
                error.message);
        std::vector<std::string> names;
        Check(
            ListMotionKitNames(location, names, error) &&
                names == std::vector<std::string>{"Bleu"},
            "the folder's kit is listed under its own name: " + error.message);
    }
}

// CH3 (review) -- a rewrite keeps the file's permissions, which the rename
// of a new file would otherwise replace.
void CheckKitFileMode() {
    TempLibrary library("mode");
    const MotionLibraryLocation& location = library.location;
    const fs::path file = library.Kit("marque");
    MotionKitSaved saved;
    MotionKitError error;
    Check(SaveMotionKitVersion(location, "marque", Parsed(kMarque), 0, "panel",
                               saved, error),
          "v1: " + error.message);
    const auto mode = [&file]() {
        return fs::status(file).permissions() & fs::perms::all;
    };
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    Check(EditMotionKitVersion(location, "marque",
                               {Set("description", R"("Réglée.")")}, 1, "panel",
                               saved, error) &&
              saved.created && saved.version == 2 &&
              mode() == (fs::perms::owner_read | fs::perms::owner_write),
          "a file set to 0600 stays 0600 after an edit: " + error.message);
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read);
    Check(RestoreMotionKitVersion(location, "marque", 1, 2, saved, error) &&
              saved.created && saved.version == 3 &&
              mode() == (fs::perms::owner_read | fs::perms::owner_write |
                         fs::perms::group_read),
          "and 0640 stays 0640 after a restore: " + error.message);
}

// CH3 (review) -- the temporary files a write cut short left in kits/: this
// kit's, an hour old, are removed by its next write; a younger one (another
// process's write in progress) and other kits' are left.
void CheckStaleTemporaries() {
    TempLibrary library("leftovers");
    const MotionLibraryLocation& location = library.location;
    MotionKitSaved saved;
    MotionKitError error;
    Check(SaveMotionKitVersion(location, "marque", Parsed(kMarque), 0, "panel",
                               saved, error),
          "v1: " + error.message);
    const fs::path kits = library.root / "kits";
    const fs::path old = kits / ".marque.json.tmp-1-1";
    const fs::path young = kits / ".marque.json.tmp-1-2";
    const fs::path longer = kits / ".marque-2.json.tmp-1-1";
    const fs::path other = kits / ".rose.json.tmp-1-1";
    for (const fs::path& path : {old, young, longer, other}) {
        WriteFile(path, "{");
    }
    const auto backdate = [](const fs::path& path) {
        struct timeval times[2] = {};
        times[0].tv_sec = ::time(nullptr) - 2 * 60 * 60;
        times[1].tv_sec = times[0].tv_sec;
        return ::utimes(path.c_str(), times) == 0;
    };
    Check(backdate(old) && backdate(longer) && backdate(other),
          "three temporary files two hours old");
    Check(EditMotionKitVersion(location, "marque",
                               {Set("description", R"("Réglée.")")}, 1, "panel",
                               saved, error) &&
              saved.created,
          "an edit of marque: " + error.message);
    Check(!fs::exists(old) && fs::exists(young) && fs::exists(longer) &&
              fs::exists(other),
          "marque's old temporary file is gone; its young one, marque-2's "
          "and rose's are kept");
}

// MB10 is kept for pieces: a piece file whose content the base held is not
// imported again, even when a later version was saved by the shelf.
void CheckPiecesKeepMb10() {
    TempLibrary library("pieces");
    const MotionLibraryLocation& location = library.location;
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog: " + error);
    const MotionTemplateInfo* carton = nullptr;
    for (const MotionTemplateInfo& piece : shipped) {
        if (piece.name == "carton") {
            carton = &piece;
        }
    }
    if (carton == nullptr) {
        Check(false, "carton is shipped");
        return;
    }
    MotionTemplateInfo piece = *carton;
    piece.name = "bande";
    piece.description = "Une bande.";
    const std::string first = DumpMotionPiece(piece);
    WriteFile(library.root / "bande.json", first);
    MotionLibraryContents contents;
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.pieces.size() == 1 && contents.pieces[0].version == 1,
          "the piece file enters as v1");
    piece.description = "Une bande, rangée par l'outil.";
    int64_t version = 0;
    bool existed = false;
    Check(SaveMotionPieceVersion(location, piece, true, version, existed,
                                 error) &&
              version == 2,
          "the shelf saves v2: " + error);
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.imported.empty() && contents.pieces[0].version == 2 &&
              ReadFile(library.root / "bande.json") == first,
          "the file, an older content, is not imported again, and the engine "
          "does not touch it");
}

}  // namespace

int main() {
    CheckDumpStable();
    CheckDumpRoundTrip();
    CheckEditPaths();
    CheckEditAtomic();
    CheckEditNoop();
    CheckFormatUpgrade();
    CheckKitNumber();
    CheckParseEdits();
    CheckKitSave();
    CheckBrokenLatest();
    CheckKitNamesAndFiles();
    CheckKitWriteFailure();
    CheckKitNameCase();
    CheckKitFileMode();
    CheckStaleTemporaries();
    CheckPiecesKeepMb10();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "motion kit edit tests passed\n";
    return 0;
}
