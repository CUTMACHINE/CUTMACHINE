#pragma once

// M5 -- docs/roadmap/MOTION-2026-09.md. Templates for the editing agent: a
// name on screen, a card and a logo, as compositions of the common format --
// data, not tools coded apart. They live under assets/motion-templates and
// are embedded in the binary (MotionTemplatesData.cc, generated), so every
// surface lists the same ones.
//
// A template declares its formats (9x16, 16x9, 1x1). A clip of motion shows
// its composition at the composition's own size, placed like any picture,
// so a template put on a 9:16 timeline has to BE a 9:16 composition: adding
// one to a project instantiates it in the format whose size is the
// timeline's, with that format's prop values as the defaults, and without
// the other formats -- whose values would otherwise disagree with the new
// defaults (kept on demand since MB1, rebased so that they agree: see
// InstantiateMotionTemplate). A timeline no format fits is refused, by
// name, with the sizes the template knows.
//
// Burned-in subtitles are not a template: the caption tracks already burn
// them in at export, styled (ITW4), and a second way to draw a subtitle
// would be a second answer to where its words fall.
//
// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. Three embedded
// templates cannot grow: adding one meant editing the build. A template is
// now a *piece*, a file that carries what it is for and what must still
// hold once it is adapted around its composition, and the catalog reads
// two shelves: the pieces the binary ships, and a library directory on
// this machine that grows without a build. A project takes a copy of the
// piece it adds, so it never depends on the library it came from.
//
// Portable: no AppKit, no FFmpeg (tests/motion_templates_tests.cc).

#include "Json.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The `format` of a piece file. A file that says anything else is refused
// rather than half-read.
inline constexpr char kMotionPieceFormat[] = "motion-piece.v1";

// The embedded sources, name and piece file, in declaration order.
const std::vector<std::pair<std::string, std::string>>&
EmbeddedMotionTemplateSources();

struct MotionTemplateInfo {
    std::string name;
    // What the template is for, for the agent choosing one.
    std::string description;
    // MB1 -- when it fits and when it does not, in a sentence each, and
    // what must still be true once the piece is adapted: a piece is a
    // mechanism, and these say which part of it is the mechanism. All
    // optional.
    std::string use;
    std::string avoid;
    std::vector<std::string> invariants;
    // Where the piece comes from: the film it was made for, a licence.
    std::string origin;
    // The file of a library piece; empty for one the binary ships.
    std::string path;
    // MB10 -- its version in the personal base (MotionLibraryBase.h); 0 for
    // a piece the binary ships or one read from a file.
    int64_t version = 0;
    mcp_json::Value definition;
};

// A file of the library that is not a piece, and why. A broken file must
// not empty the shelf beside it, and must not go unnoticed either.
struct MotionLibraryIssue {
    std::string path;
    std::string message;
};

// Letters, digits, '-' and '_', starting with a letter or a digit, 64 at
// most: a piece's name is its file's, so it must not be a path.
bool IsMotionPieceName(const std::string& name);

// DEC11 -- docs/roadmap/MOTION-WEB-2026-10.md. Whether `piece` answers a
// search of the shelf: every word of `query` appears in its name,
// description, use, avoid, invariants or origin, as a substring, ignoring
// case and French accents (FoldShotText, the rule search_shots follows:
// "entree ECRAN" finds "Entrée à l'écran"). Words are cut at spaces; an
// empty query, or one of spaces only, matches every piece. The shelf was
// listed whole, which twenty pieces allow and three hundred do not (the
// web library of MW4): the agent narrows it before it reads it.
bool MotionPieceMatches(const MotionTemplateInfo& piece,
                        const std::string& query);

// One piece file. Refused, with the reason: JSON that is not a piece, a
// field the format does not know, no description, a composition the motion
// parser refuses, one that declares no format (it could never be added) or
// one that still carries identities (each addition draws its own).
bool ParseMotionPiece(const std::string& name, const std::string& json,
                      MotionTemplateInfo& piece, std::string& error);

// The piece as its file spells it, indented for a person to read and for a
// diff to show one change per line. ParseMotionPiece reads it back.
std::string DumpMotionPiece(const MotionTemplateInfo& piece);

// The catalog the binary ships: every embedded piece, parsed. One that does
// not parse is a build error in spirit; here it is refused by name, so a
// broken asset cannot pass for an empty catalog.
bool MotionTemplateCatalog(std::vector<MotionTemplateInfo>& templates,
                           std::string& error);

// MB10 -- the library itself lives in the user's personal base
// (MotionLibraryBase.h); a folder of piece files is its inbox, read below.
//
// The pieces of `directory`, by name: each `<name>.json` directly in it. A
// directory that does not exist is an empty library. A file that is not a
// piece, or whose name is one of `shipped`, lands in `issues` and the rest
// is still read: a library piece never replaces a shipped one, so a name
// means the same thing on every machine.
void ReadMotionLibrary(const std::string& directory,
                       const std::vector<MotionTemplateInfo>& shipped,
                       std::vector<MotionTemplateInfo>& pieces,
                       std::vector<MotionLibraryIssue>& issues);

// A stored composition's definition as the composition of a piece: its
// identities removed, and a format added for its own size when it declares
// none of that size, so that the timeline it was built for can take it
// back. Refused when a node plays a media of the project written by its
// ULID (one bound to a prop passes: each project sets it) or instantiates
// another of its compositions: a piece carries nothing of the project it
// leaves.
bool MotionPieceDefinition(const mcp_json::Value& stored,
                           mcp_json::Value& definition, std::string& error);

// MB10 -- a piece file, as a generator deposits one in the inbox; the
// shelf itself saves into the base (SaveMotionPieceVersion).
//
// Writes `piece` as `<directory>/<name>.json`, creating the directory. An
// existing file is only replaced with `overwrite`; `existed` says there was
// one either way. The file appears whole or not at all.
bool WriteMotionPiece(const std::string& directory,
                      const MotionTemplateInfo& piece, bool overwrite,
                      std::string& path, bool& existed, std::string& error);

// MB4 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A client's charter as
// data: values for the props the pieces share by name, applied to a
// neutral piece when it is added. The shipped pieces stay neutral; a kit
// lives in the machine's library (kits/<name>.json), never in the
// repository, and a project keeps the values it was given, not the kit.
inline constexpr char kMotionKitFormat[] = "motion-kit.v1";

// The props a kit can set: the names every piece that has one of these
// gives it, with the type its prop declares.
struct MotionKitRole {
    const char* name;
    // "color" or "text", as a prop declares its type.
    const char* type;
    const char* meaning;
};

// accent, texte, fond (colors), police (text, bound to fontFamily).
const std::vector<MotionKitRole>& MotionKitRoles();

// DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md (MB4, decided on 23
// October 2026). The kit grows into a brand's charter: one file per brand,
// in the same kits/ folder and the same base, that covers the motion (its
// props, as motion-kit.v1 does), the captions (a caption style, per format
// when the sizes differ) and every colour and font the brand allows, so
// that an export can be checked against it (TimelineCheck.h, `charter`).
// A new format rather than new fields in motion-kit.v1: a binary that
// predates DEC6 refuses the file instead of applying half of it and never
// checking the rest. A motion-kit.v1 file still reads, as a charter of its
// props alone.
inline constexpr char kBrandKitFormat[] = "brand-kit.v1";

struct MotionKitInfo {
    std::string name;
    std::string description;
    // The graphic principle, in sentences, for whoever adapts the piece:
    // what no value says (a bar under every title, never a gradient...).
    std::string principle;
    // Role name to value, roles of MotionKitRoles only. Optional in a
    // brand-kit.v1 file (a charter may cover only the captions).
    mcp_json::Value props;
    std::string path;
    // MB10 -- its version in the personal base; 0 when read from a file.
    int64_t version = 0;
    // DEC6 -- kMotionKitFormat or kBrandKitFormat, as the file says.
    std::string format;
    // DEC6 -- the colours the brand allows beyond those its props and its
    // captions name, each "#rrggbb" as the file writes it.
    std::vector<std::string> palette;
    // DEC6 -- the font families the brand allows beyond props.police and
    // the captions' font_family.
    std::vector<std::string> fonts;
    // DEC6 -- the caption style, as add_caption_style takes its settings
    // (font_family, font_size, font_weight, color, position, outline_width,
    // outline_color, shadow_offset, shadow_color, background_color,
    // background_padding, vertical_position), plus `formats`: by name, a
    // width, a height and the settings that differ at that size. Null when
    // the charter says nothing of captions.
    mcp_json::Value captions;
    // DEC6 -- where the values come from (the charter's PDF and its page, a
    // delivered film), in sentences: a charter whose values cannot be traced
    // is a guess.
    std::vector<std::string> sources;
    // M5b -- docs/roadmap/MOTION-2026-09.md. By piece name, the values the
    // charter gives that piece's own props, beyond the shared roles: the
    // second line of `nom` in colour on white, the brand's logo file for
    // `logo` (the author, 6 November 2026). The shipped piece stays neutral;
    // the charter says how this brand wears it. Null when the charter names
    // none. Each value is checked against the prop it names when the piece
    // is added (ApplyMotionKit): only then are its props known.
    mcp_json::Value pieces;
    // M5b -- by piece name, why the charter does not offer that piece, in
    // the words of whoever decided it (the author on a brand's charter, 6
    // November 2026: `carton` "pas besoin", `maillage` withdrawn). Adding
    // the piece with this charter is refused with the reason; without the
    // charter, it is added knowingly. Null when the charter withdraws none.
    mcp_json::Value withdrawn;
};

// One kit file. Refused, with the reason: not JSON, a field the format does
// not know, no description, no props, a prop that is not a role, or a value
// its role's type does not take (a color the motion parser refuses).
// DEC6 -- a brand-kit.v1 file also: a palette colour that is not "#rrggbb",
// an empty font, a caption setting the caption style does not have or a
// value it refuses, a caption format without its size, and a charter that
// sets nothing at all. M5b -- `pieces`: a name that is not a piece's, an
// empty section, a value that is not a string, a number or a boolean, an
// empty string, or a shared role given a value its type does not take;
// `withdrawn`: a name that is not a piece's, an empty reason, or a piece
// both withdrawn and given values in `pieces`.
bool ParseMotionKit(const std::string& name, const std::string& json,
                    MotionKitInfo& kit, std::string& error);

// DEC6 -- every colour the charter allows, as "#rrggbb" in lower case, in
// first-seen order: its colour props, its palette, its captions' colours
// (base and formats), and (M5b) every value of `pieces` that reads as a
// colour. Alpha is not a colour: "#000000@0.5" allows #000000.
std::vector<std::string> MotionKitColors(const MotionKitInfo& kit);

// DEC6 -- every font family the charter allows, in first-seen order: its
// `police` prop, its fonts, its captions' font_family (base and formats),
// and (M5b) the `police` of each piece in `pieces`.
std::vector<std::string> MotionKitFonts(const MotionKitInfo& kit);

// DEC6 -- "#rrggbb" in lower case for a colour of the motion format
// (#rgb, #rrggbb, #rrggbbaa, rgb(), rgba(), white, black); empty for one
// fully transparent or not a colour. What the charter check compares.
std::string MotionColorKey(const std::string& text);

struct CaptionStyle;

// DEC6 -- the charter's caption style for a timeline of `width` x
// `height`: the defaults of a new style, the charter's settings over them,
// then those of the caption format of exactly that size. `format` receives
// that format's name (empty when the charter declares none), `set` the
// settings the charter gave. Refused, with the sizes it knows, when the
// charter declares caption formats and none is of that size: its sizes are
// pixels of a frame, and another frame would read them wrong. Refused too
// when the charter has no captions. `style.id` is left as it was.
bool MotionKitCaptionStyle(const MotionKitInfo& kit, int32_t width,
                           int32_t height, CaptionStyle& style,
                           std::string& format, std::vector<std::string>& set,
                           std::string& error);

// Where the kits of a library live: `<library>/kits`.
std::string MotionKitDirectory(const std::string& library);

// The kits of `library`, by name, each `kits/<name>.json`. A directory that
// does not exist holds none; a file that is not a kit lands in `issues`.
void ReadMotionKits(const std::string& library,
                    std::vector<MotionKitInfo>& kits,
                    std::vector<MotionLibraryIssue>& issues);

// M5b -- why `kit` does not offer the piece `piece`; empty when it does.
std::string MotionKitWithdrawal(const MotionKitInfo& kit,
                                const std::string& piece);

// What a kit did to a piece: the props it set, and those it names that the
// piece does not have (a card without an accent): said, not refused.
struct MotionKitApplication {
    std::vector<std::string> applied;
    std::vector<std::string> not_in_piece;
};

// `definition`, the composition of the piece named `piece`, with the kit's
// values as the defaults of the props of that name and as their value in
// every format that sets them: a charter holds in each format. A variant's
// own value stays -- a version is a choice made on purpose. Refused when a
// prop of that name is of another type than its role.
//
// M5b -- then the charter's `pieces[piece]`, each value on the prop it
// names, over a shared role that set it too. Refused, by name, when the
// piece has no such prop or the prop does not take the value (a colour
// that does not read, a number out of its bounds, a word outside an enum):
// a charter written for another version of the piece is said, not
// half-applied. Refused too, with the charter's reason, when the charter
// withdraws the piece.
bool ApplyMotionKit(const MotionKitInfo& kit, const std::string& piece,
                    mcp_json::Value& definition,
                    MotionKitApplication& application, std::string& error);

// `definition` instantiated for a timeline of `width` x `height`: the
// format of exactly that size becomes the composition's size and its props
// the defaults; `formats` goes. `format` receives the format's name.
//
// MB1 -- with `keepFormats` the formats stay, rebased on the chosen one:
// it keeps its size and loses its values, now the defaults, and every other
// format gains the template's own default for each prop the chosen one set
// and it did not. Every format then resolves to the values it had in the
// piece, so the instance renders in each of them and goes back to the
// shelf (MotionPieceDefinition) with all of them. Not the default: a
// composition that declares formats renders in each of them unless told
// otherwise (MotionVideo.h), and a film on one timeline wants one.
bool InstantiateMotionTemplate(const mcp_json::Value& definition, int32_t width,
                               int32_t height, mcp_json::Value& instance,
                               std::string& format, std::string& error,
                               bool keepFormats = false);
