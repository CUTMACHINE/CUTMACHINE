#pragma once

// MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md (DEC43). An After
// Effects composition, read through the bridge (read_comp.jsx,
// after-comp.v1), becomes a composition of the format (docs/MOTION_FORMAT.md)
// that a person can read: one node per layer, named after it, never one per
// letter. The export (MotionAfterExport.h) goes the other way, by the same
// rule: a translation is admitted when both sides draw the same pixels;
// what has no equivalent is named in the report, never dropped in silence
// and never guessed.
//
// What After computes and the format cannot say -- an expression, a curved
// trajectory, a track matte -- is read as After gives it, one value per
// frame, and the report says the property is no longer live. Text is placed
// on its baseline by measuring it with the same measurer that renders it.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

// A face as the format names it: what fontFamily, fontWeight and fontStyle
// resolve back to the PostScript name After gave.
struct MotionAfterFont {
    std::string family;
    int weight = 400;
    bool italic = false;
};

// Resolves a PostScript name to the face the measurer draws; false when no
// face of this machine has that name.
using MotionAfterFontResolver =
    std::function<bool(const std::string& postScriptName, MotionAfterFont&)>;

struct MotionAfterImportOptions {
    // PostScript name read in After -> PostScript name drawn instead, like
    // After's own font replacement: a face this machine lacks. Each use is
    // named in the report.
    std::map<std::string, std::string> fonts;
};

struct MotionAfterImportNote {
    // The layer's name, then the part of it the note is about.
    std::string layer;
    std::string note;
};

struct MotionAfterImportReport {
    std::string comp_name;
    int layers = 0;
    int nodes = 0;
    int keys = 0;
    std::vector<MotionAfterImportNote> notes;
};

// `comp` is the "comp" object of an after-comp.v1 result (or the whole
// bridge response, or its result). The definition is a composition object
// ready for add_motion_composition. Refused (InvalidValue) when `comp` is
// not an after-comp.v1 reading, or when the definition the import writes
// does not parse -- a bug of the import, never handed on.
bool ImportAfterComposition(const mcp_json::Value& comp,
                            const MotionAfterImportOptions& options,
                            const MotionAfterFontResolver& fonts,
                            MotionMeasurer& measurer,
                            mcp_json::Value& definition,
                            MotionAfterImportReport& report,
                            MotionError& error);

// The report as the CLI and import_after_comp give it.
void MotionAfterImportReportFields(const MotionAfterImportReport& report,
                                   mcp_json::Value& result);

// What a surface hands the importer: the reading, the options, the font
// directories to register first.
struct MotionAfterImportRequest {
    const mcp_json::Value* comp = nullptr;
    MotionAfterImportOptions options;
    std::vector<std::string> font_directories;
};

// Measuring text and resolving a PostScript name need Core Text, which the
// model does not link: the executable installs the importer beside the
// describer of the export (MotionAfterExport.h); null in a process that
// links the model alone, where import_after_comp is then refused.
using MotionAfterImporter = bool (*)(const MotionAfterImportRequest& request,
                                     mcp_json::Value& definition,
                                     MotionAfterImportReport& report,
                                     MotionError& error);

namespace motion_after_importer {
inline std::atomic<MotionAfterImporter>& Slot() {
    static std::atomic<MotionAfterImporter> importer{nullptr};
    return importer;
}
}  // namespace motion_after_importer

inline void SetMotionAfterImporter(MotionAfterImporter importer) {
    motion_after_importer::Slot().store(importer);
}

inline MotionAfterImporter CurrentMotionAfterImporter() {
    return motion_after_importer::Slot().load();
}
