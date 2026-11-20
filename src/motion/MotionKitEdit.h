#pragma once

// CH1 -- docs/roadmap/CHARTE-2026-11.md. A charter (kits/<name>.json,
// motion-kit.v1 or brand-kit.v1, MotionTemplates.h) could only be read: it
// was written by hand, and nothing in the engine could write one back. The
// panel, the agent and a person in a text editor now all edit the same
// file (DEC60), so the engine needs a written form of a charter and a way
// to change one value of it.
//
// Both work on the parsed JSON tree (mcp_json::Value), never on
// MotionKitInfo: the tree keeps the order of the keys, every number as its
// literal ("0.50" stays "0.50") and the UTF-8 of every string, where
// MotionKitInfo loses the order of the top level. A gesture in the panel
// then changes one line of the file, and a diff reads as what was done.
//
// An edit names one value by a bounded path, not by a JSON Pointer: the
// paths are the places a charter has, so an edit cannot plant a field the
// format does not know, and every result is judged again by ParseMotionKit
// before anyone writes it.
//
// Portable: no framework, no I/O (tests/motion_kit_edit_tests.cc).

#include "Json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Why a charter was not edited, read or written, in the shape of
// MotionError: a code a caller can branch on, the path concerned (a kit
// path for an edit, a file for I/O; empty when the whole is concerned) and
// a sentence.
struct MotionKitError {
    std::string code;
    std::string path;
    std::string message;
};

// The codes. InvalidKit: the result, or a file given whole, is refused by
// ParseMotionKit (the message is its reason). InvalidEdit: an edit outside
// the grammar below, or one that cannot be made (an index past the end, a
// parent that is not an object). StaleKit: the latest version is not the
// one the writer read (MotionLibraryBase.h). KitFileDiverged: kits/<name>
// .json no longer reads as a charter, and the engine will not overwrite it.
// UnknownKit: no charter, or no such version, of that name. IoError: the
// base or the file could not be read or written.
inline constexpr char kMotionKitInvalidKit[] = "InvalidKit";
inline constexpr char kMotionKitInvalidEdit[] = "InvalidEdit";
inline constexpr char kMotionKitStaleKit[] = "StaleKit";
inline constexpr char kMotionKitFileDiverged[] = "KitFileDiverged";
inline constexpr char kMotionKitUnknownKit[] = "UnknownKit";
inline constexpr char kMotionKitIoError[] = "IoError";

// The charter as its file spells it: the indented form of DumpMotionPiece
// (DumpIndented in MotionTemplates.cc, which CH1 exposes for this), the
// keys in the tree's order, the number literals as they are, then one
// "\n". Idempotent: dumping what this dump parses to gives the same bytes.
// It is the only form the engine writes into kits/, and the text two
// versions are compared by (MotionLibraryBase.h: no duplicate version).
std::string DumpMotionKit(const mcp_json::Value& file);

// One change to a charter. `value` set: the value at `path` becomes it;
// nullopt: the value at `path` is removed. `insert` (a list item only):
// the value goes in before <index> instead of replacing it; <index> may
// then be the list's size, which appends.
//
// The paths, dot-separated, each segment [A-Za-z0-9_-]{1,64}, an <index>
// a decimal without a leading zero:
//
//   format                      only to "brand-kit.v1", never removed
//   description | principle
//   props.<role>
//   palette | fonts | sources   the whole list
//   palette.<index> | fonts.<index> | sources.<index>
//   captions                    the whole caption style
//   captions.<setting>          <setting> != "formats"
//   captions.formats.<f>        one caption format, whole
//   captions.formats.<f>.<setting> | .width | .height
//   pieces.<p>                  every value of a piece, whole
//   pieces.<p>.<prop>           <prop> != "formats": every format
//   pieces.<p>.formats.<f>      DEC62: one format of a piece, whole
//   pieces.<p>.formats.<f>.<prop>   DEC62: that format only
//   withdrawn.<p>
//
// Whether a role, a setting, a piece or a value exists or fits is
// ParseMotionKit's to say, on the result; the grammar only bounds where an
// edit may write.
struct MotionKitEdit {
    std::string path;
    std::optional<mcp_json::Value> value;
    bool insert = false;
};

// `file` with every edit of `edits` made, in order, into `output`; all of
// them or none. A value set where there is one replaces it in place (the
// key keeps its rank); a new key goes last in its object; missing parents
// are created as objects, and a missing list by an insert at index 0 (the
// first colour of a palette that has none). A removal of what is not there
// changes nothing; an object or a list a removal leaves empty is removed
// from its parent in turn, up to the top level (ParseMotionKit refuses an
// empty `pieces`, `pieces.<p>`, `formats` or `withdrawn`, and an empty
// `props` in a brand-kit.v1). A null value is refused (InvalidEdit): a
// value is removed, not set to null.
//
// A charter path (palette, fonts, sources, captions, pieces, withdrawn) on
// a motion-kit.v1 is refused (InvalidEdit, naming the format to write),
// a removal as well as a set, unless the same batch sets `format` to
// "brand-kit.v1".
//
// The result is then judged by ParseMotionKit under `name` (InvalidKit
// with its reason; `path` is the edit's when the batch has one edit, empty
// otherwise). `changed` is false when DumpMotionKit of the result equals
// DumpMotionKit of `file`: the caller writes no version for it.
bool EditMotionKit(const std::string& name, const mcp_json::Value& file,
                   const std::vector<MotionKitEdit>& edits,
                   mcp_json::Value& output, bool& changed,
                   MotionKitError& error);

// The edits a tool receives, as JSON: an array of objects, each with
// `path` (a string) and either `value` (any JSON but null) or
// `remove: true`, and optionally `insert: true` with a `value`. Any other
// key, an empty array, or an item that is neither a set nor a removal is
// refused (InvalidEdit, `path` naming the item: "edits[2]"). The paths
// are checked by EditMotionKit, not here.
bool ParseMotionKitEdits(const mcp_json::Value& json,
                         std::vector<MotionKitEdit>& out,
                         MotionKitError& error);

// The path of a piece's own prop: `pieces.<piece>.<prop>`, every format,
// when `format` is empty; `pieces.<piece>.formats.<format>.<prop>` (DEC62)
// otherwise, `format` being the piece's own name for it.
inline std::string MotionKitPiecePath(const std::string& piece,
                                      const std::string& format,
                                      const std::string& prop) {
    std::string path = "pieces." + piece + ".";
    if (!format.empty()) {
        path += "formats." + format + ".";
    }
    return path + prop;
}

// The path of a caption setting: `captions.<key>` when `format` is empty,
// `captions.formats.<format>.<key>` otherwise.
inline std::string MotionKitCaptionPath(const std::string& format,
                                        const std::string& key) {
    return format.empty() ? "captions." + key
                          : "captions.formats." + format + "." + key;
}

// The value at `path` in `file`: objects by key, arrays by an index
// written as the grammar writes it. Null when any segment is missing,
// empty, or names into a scalar; an empty path finds nothing. Any path is
// looked up, in the grammar or not: this reads, it judges nothing.
inline const mcp_json::Value* FindMotionKitPath(const mcp_json::Value& file,
                                                const std::string& path) {
    const mcp_json::Value* at = &file;
    size_t start = 0;
    while (at != nullptr) {
        const size_t dot = path.find('.', start);
        const std::string segment = path.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (segment.empty()) {
            return nullptr;
        }
        if (at->IsObject()) {
            at = at->Find(segment);
        } else if (at->IsArray()) {
            if (segment.size() > 9 ||
                (segment.size() > 1 && segment[0] == '0')) {
                return nullptr;
            }
            size_t index = 0;
            for (const char digit : segment) {
                if (digit < '0' || digit > '9') {
                    return nullptr;
                }
                index = index * 10 + static_cast<size_t>(digit - '0');
            }
            at = index < at->AsArray().size() ? &at->AsArray()[index] : nullptr;
        } else {
            return nullptr;
        }
        if (dot == std::string::npos) {
            return at;
        }
        start = dot + 1;
    }
    return nullptr;
}

// `value` rounded to a whole number of `step` (none when `step` is not
// positive), as a number literal spelt like MotionNumberValue
// (MotionCanvas.h): rounded to a thousandth, the shortest decimal that
// reads back to the same double, an integer without a point, never "-0".
// 12.3 at 0.5 is "12.5"; 12 is "12", not "12.0". Never Value::MakeDouble,
// which writes std::to_string ("12.500000") into the file. A step whose
// inverse is a whole number (0.5, 0.1) divides by that inverse, so that
// 12.3 at 0.1 is the double nearest 12.3, not 123 * 0.1. A non-finite
// value is spelt 0: JSON has no literal for it, and a drag never makes one.
inline mcp_json::Value MotionKitNumber(double value, double step) {
    double rounded = std::isfinite(value) ? value : 0.0;
    if (step > 0.0 && std::isfinite(step)) {
        const double inverse = 1.0 / step;
        const double whole = std::round(inverse);
        rounded = whole >= 1.0 && std::abs(inverse - whole) < 1e-9
                      ? std::round(rounded * whole) / whole
                      : std::round(rounded / step) * step;
    }
    rounded = std::round(rounded * 1000.0) / 1000.0;
    if (rounded == 0.0 || !std::isfinite(rounded)) {
        rounded = 0.0;
    }
    char buffer[64];
    const auto written =
        std::to_chars(buffer, buffer + sizeof(buffer), rounded);
    return mcp_json::Value::MakeRawNumber(std::string(buffer, written.ptr));
}
