#pragma once
// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. What the
// files of the MCP catalog share: the JSON Schema fragment builders,
// the argument readers and the few helpers two domains use. Defined in
// McpTools.cc. Internal to the catalog: only McpTools*.cc include it,
// with McpTools.cc's own includes, so that a tool reads the same names
// it read before the split.
#include "McpTools.h"

#include "AfterBridge.h"
#include "BoundaryAir.h"
#include "CaptionBreakModel.h"
#include "CaptionGeneration.h"
#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipGradeCopy.h"
#include "ClipSpeed.h"
#include "ColorCdl.h"
#include "ColorEffects.h"
#include "ColorMeasure.h"
#include "CutInspection.h"
#include "CutInspectionRender.h"
#include "CutReview.h"
#include "CutWords.h"
#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "EditPlan.h"
#include "Edl.h"
#include "EngineLog.h"
#include "FfmpegCapabilities.h"
#include "FontCatalog.h"
#include "Hesitations.h"
#include "Interchange.h"
#include "LookLut.h"
#include "McpGenerationTools.h"
#include "McpMotionClipTools.h"
#include "McpMotionTools.h"
#include "MfaAlignment.h"
#include "MotionLibraryBase.h"
#include "Multicam.h"
#include "MusicStructure.h"
#include "PauseTightening.h"
#include "Procedures.h"
#include "ProjectArchive.h"
#include "RationalTime.h"
#include "SeparateSound.h"
#include "SequenceFormat.h"
#include "ShotDescriptions.h"
#include "SoundRender.h"
#include "SoundStem.h"
#include "SourceAddress.h"
#include "SpeechSelection.h"
#include "SubjectTracking.h"
#include "Subtitles.h"
#include "TempoGrid.h"
#include "TimelineCheck.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"
#include "TimelineView.h"
#include "TimelineWords.h"
#include "TransitionRender.h"
#include "Ulid.h"
#include "WordBoundaries.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace mcp_tools {

using mcp_json::Value;

// ---------------------------------------------------------------------
// JSON Schema fragment builders. Schemas are static per tool, so these are
// just string assembly -- the runtime validation that actually matters
// (unknown keys, non-finite numbers, ID resolution) lives in the ReadXxx
// helpers below and runs against the caller's arguments, not the schema.
// ---------------------------------------------------------------------

std::string Esc(const std::string& text);

std::string StringSchema(const std::string& description);

// PRD15 -- ROADMAP.md. Said on 141 arguments: only what the argument names.
// That a prefix or an alias from describe works too (IdResolver.h) is said
// once, in the server's instructions. A list says it once, not on its items.
std::string IdSchema(const std::string& description);

std::string IdArraySchema(const std::string& description);

std::string BoolSchema(const std::string& description);

std::string IntSchema(const std::string& description);

std::string PositiveIntSchema(const std::string& description);

std::string EnumSchema(const std::vector<std::string>& values,
                       const std::string& description);

// PRD15 -- on 53 arguments: what value/rate means, and nothing more. The
// schema says both are integers and the rate at least 1; ReadTimeValue
// refuses a float, so the text no longer says so.
extern const char kTimeSchemaText[];

extern const char kFractionSchemaText[];

// The same time, with what it names before the unit.
std::string TimeSchema(const std::string& description);

// The same fraction, with what it names instead of the generic text.
std::string FractionSchema(const std::string& description);

// G1, G2 -- a rectangle of a measured picture.
extern const std::string kColorRegionSchema;

std::string ArraySchema(const std::string& itemSchema,
                        const std::string& description);

// IA9 -- ROADMAP.md. Every tool that cuts sound says so the same way.
extern const char kCutWordsNote[];

// Builds one tool's top-level `inputSchema` object and, incidentally, the
// list of keys CheckKnownKeys() should accept -- the two must always agree,
// so both come from the same Field() calls.
class SchemaBuilder {
public:
    SchemaBuilder& Field(const std::string& name, const std::string& schema,
                         bool required) {
        if (!properties_.empty()) properties_ += ",";
        properties_ += "\"" + name + "\":" + schema;
        if (required) required_.push_back(name);
        return *this;
    }

    // PRD15 -- an empty `required` is left out: JSON Schema and the MCP
    // specification read its absence the same way.
    std::string Build(const std::string& description) const {
        std::string requiredJson;
        for (size_t index = 0; index < required_.size(); ++index) {
            requiredJson += index ? ",\"" : ",\"required\":[\"";
            requiredJson += required_[index] + "\"";
        }
        if (!requiredJson.empty()) requiredJson += "]";
        return "{\"type\":\"object\",\"description\":\"" + Esc(description) +
               "\",\"properties\":{" + properties_ + "}" + requiredJson +
               ",\"additionalProperties\":false}";
    }

private:
    std::string properties_;
    std::vector<std::string> required_;
};

// ---------------------------------------------------------------------
// Argument readers. Every one names the offending field ("tool.field" or
// "tool.field[index]") in `message` on failure. None of them throw.
// ---------------------------------------------------------------------

bool IsTimelineEditingTool(const std::string& name);

// PL3 -- ROADMAP.md. Tools that read one timeline. Each takes an optional
// timeline_id to read another one without activating it
// (McpBackend::SelectTimelineForRead): preparing a plan for a timeline that
// is not the active one starts by reading it, and switching the active
// timeline to read it was a side effect nobody asked for.
bool IsTimelineReadingTool(const std::string& name);

bool CheckKnownKeys(const Value& args, const std::vector<std::string>& allowed,
                    const std::string& path, std::string& message);

bool ReadString(const Value& args, const std::string& key,
                const std::string& path, bool required,
                const std::string& defaultValue, std::string& out,
                std::string& message);

bool ReadBool(const Value& args, const std::string& key,
              const std::string& path, bool defaultValue, bool& out,
              std::string& message);

bool ReadInt64(const Value& args, const std::string& key,
               const std::string& path, bool required, int64_t defaultValue,
               int64_t& out, std::string& message);

bool ReadInt32(const Value& args, const std::string& key,
               const std::string& path, bool required, int32_t defaultValue,
               int32_t& out, std::string& message);

bool ReadTimeValue(const Value& field, const std::string& path,
                   RationalTime& out, std::string& message);

bool ReadTime(const Value& args, const std::string& key,
              const std::string& path, bool required, RationalTime& out,
              std::string& message);

bool ReadFractionValue(const Value& field, const std::string& path,
                       int32_t& num, int32_t& den, std::string& message);

bool ReadId(const Value& args, const std::string& key, const std::string& path,
            const IdResolver& resolver, bool required, Ulid& out,
            std::string& message);

// An optional ID that may be explicitly empty ("no parent bin", "clear
// caption"): an empty string is accepted as-is, never resolved.
bool ReadOptionalIdOrEmpty(const Value& args, const std::string& key,
                           const std::string& path, const IdResolver& resolver,
                           Ulid& out, std::string& message);

bool ReadIdArray(const Value& args, const std::string& key,
                 const std::string& path, const IdResolver& resolver,
                 std::vector<Ulid>& out, std::string& message);

bool Fail(std::string& errorName, std::string& message, std::string text);

// MOTIF3 -- docs/roadmap/MOTIF-2026-10.md, PHILOSOPHY.md principle 7. A
// tool whose rule decides the cut records that rule itself, with the
// parameters it ran with, defaults included: what a model would have to
// remember to say, the code says. The caller's motif keeps its author, its
// note and its marker; a reason it gives must be the rule's, and its
// parameters are refused, since the tool's own are the ones that cut. Held
// for one ApplyOperation, then the call's motif is back, so a batch of
// items (RunBatch) records each under the rule.
class RuleMotifScope {
public:
    explicit RuleMotifScope(McpBackend& backend)
        : backend_(backend), saved_(backend.CurrentEditMotifs()) {}
    RuleMotifScope(const RuleMotifScope&) = delete;
    RuleMotifScope& operator=(const RuleMotifScope&) = delete;
    ~RuleMotifScope() {
        if (active_) backend_.SetEditMotifs(saved_);
    }

    bool Set(MotifReason reason, const std::string& rule, Value params,
             std::string& errorName, std::string& message) {
        EditMotif motif = saved_.motif.value_or(EditMotif{});
        if (motif.reason != MotifReason::Unstated && motif.reason != reason) {
            return Fail(errorName, message,
                        "'" + rule + ".motif.reason': " + rule +
                            " records its edits as '" +
                            MotifReasonName(reason) +
                            "'; leave the reason out and say why you ran it "
                            "in motif.note");
        }
        if (!motif.params.IsNull()) {
            return Fail(errorName, message,
                        "'" + rule + ".motif.params': " + rule +
                            " records the parameters it cut with itself");
        }
        motif.reason = reason;
        motif.rule = rule;
        motif.params = std::move(params);
        McpBackend::EditMotifs motifs = saved_;
        motifs.motif = std::move(motif);
        backend_.SetEditMotifs(std::move(motifs));
        active_ = true;
        return true;
    }

private:
    McpBackend& backend_;
    McpBackend::EditMotifs saved_;
    bool active_ = false;
};

// IA9 -- ROADMAP.md. The words a cut is about to take, measured on a dry run
// of the operation over the snapshot the dispatcher resolved it against. The
// dry run is the engine's own ApplyOperation on a copy: nothing is re-read
// after the real apply, and the project, live and in-memory backends all
// answer alike. False when the dry run is refused, in which case the real
// apply fails too and reports why.
bool MeasureCutWords(McpBackend& backend, const Document& before,
                     const Operation& operation, const TargetedWords& targeted,
                     Value& view);

// An optional array of strings, each non-empty.
bool ReadStringArray(const Value& args, const std::string& key,
                     const std::string& tool, std::vector<std::string>& out,
                     std::string& message);

// G1, G2 -- docs/roadmap/GRADE-2026-09.md. A rectangle of a measured
// picture, as exact fractions: absent leaves the region absent.
bool ReadColorRegion(const Value& args, const std::string& key,
                     const std::string& tool, ColorMeasureRegion& region,
                     std::string& message);

// ITW2 -- ROADMAP.md. Hesitations measured in the signal (Hesitations.h).
// AGENT-2026-09 A8 -- ROADMAP.md. The per-rush analyses take one media or
// a list. On every measured run the agent looped over 23 rushes in a shell
// script, one process per call -- 46 calls, and once 7 minutes stuck in
// the sandbox. A list is one call, and one background task with `async`.
// Each media is analysed in turn and reported on its own line, so one
// unreadable rush does not hide the others' results; the call as a whole
// fails only when no media was analysed.
bool ReadAnalysisMediaIds(const Value& args, const std::string& tool,
                          const IdResolver& resolver, std::vector<Ulid>& out,
                          bool& batch, std::string& message);

// Runs `analyse` on each media and, for a batch, gathers one line per
// media under `results`; a single media answers as it always has.
bool RunAnalysisBatch(const std::vector<Ulid>& mediaIds, bool batch,
                      const std::string& tool,
                      const std::function<bool(const Ulid&, std::string& json,
                                               std::string& error)>& analyse,
                      std::string& resultJson, std::string& errorName,
                      std::string& message);

Value BeatTimeValue(const RationalTime& time);

}  // namespace mcp_tools

// One per file of the catalog, called by McpToolRegistry's constructor
// in this order.
void RegisterEditTools(const McpToolAdder& add);
void RegisterSequenceTools(const McpToolAdder& add);
void RegisterClipTools(const McpToolAdder& add);
void RegisterCaptionTools(const McpToolAdder& add);
void RegisterProjectTools(const McpToolAdder& add);
void RegisterSpeechTools(const McpToolAdder& add);
void RegisterPictureTools(const McpToolAdder& add);
void RegisterColorTools(const McpToolAdder& add);
void RegisterSoundTools(const McpToolAdder& add);
void RegisterDeliveryTools(const McpToolAdder& add);
void RegisterAfterTools(const McpToolAdder& add);
// GN6b -- the local voice-over (McpVoiceTools.cc).
void RegisterVoiceTools(const McpToolAdder& add);
