// Pure-C++ subset test for the MCP server (ROADMAP.md F1.1/F1.2). Depends
// only on Document/EditLog/Operations/Json/IdResolver/McpTools/HttpServer/
// McpServer -- no ProjectStorage (CommonCrypto), no FFmpeg, no AppKit/Metal
// -- so it builds and runs on a plain Linux host, the same way
// tests/model_tests.cc and tests/edit_tests.cc do.
//
// It exercises the real HTTP + JSON-RPC transport (McpServer/HttpServer)
// end to end against an in-memory McpBackend that calls EditLog::Apply/
// Undo/Redo directly -- the exact function ApplyOperationCommand's
// `--apply-op` path calls, just without the project-file round trip. See
// tests/mcp_tests.cc for the macOS-only counterpart that goes through the
// real project file and ApplyOperationCommand byte for byte.

#include "ChatLlmClient.h"
#include "Document.h"
#include "EditLog.h"
#include "EditPlan.h"
#include "EngineLog.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpBackend.h"
#include "McpLiveBackend.h"
#include "McpServer.h"
#include "McpTools.h"
#include "MotionDocument.h"
#include "Procedures.h"
#include "ProjectArchive.h"

#include "Cli.h"
#include "Operations.h"
#include "Project.h"
#include "ShotQuality.h"
#include "SpeechOnset.h"
#include "TimelineView.h"
#include "Transcription.h"
#include "Ulid.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckFailureEnvelope(const McpToolCallOutcome& outcome,
                          const std::string& label,
                          const std::string& expectedError = std::string()) {
    mcp_json::Value envelope;
    std::string parseError;
    const bool parsed =
        mcp_json::Value::Parse(outcome.result_json, envelope, parseError) &&
        envelope.IsObject();
    Check(!outcome.ok, label + " is refused");
    Check(parsed, label + " returns a JSON object: " + parseError);
    if (!parsed) return;
    const mcp_json::Value* ok = envelope.Find("ok");
    const mcp_json::Value* error = envelope.Find("error");
    const mcp_json::Value* detail = envelope.Find("detail");
    Check(ok != nullptr && ok->IsBool() && !ok->AsBool(),
          label + " returns ok:false");
    Check(error != nullptr && error->IsString() && !error->AsString().empty() &&
              error->AsString() == outcome.error_name,
          label + " returns its stable error code");
    Check(detail != nullptr && detail->IsString() &&
              !detail->AsString().empty() &&
              detail->AsString() == outcome.message,
          label + " returns its detail");
    if (!expectedError.empty()) {
        Check(error != nullptr && error->IsString() &&
                  error->AsString() == expectedError,
              label + " preserves " + expectedError);
    }
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
        {"01K30000000000000000000005",
         "audio",
         1,
         {{"01K30000000000000000000006",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}}}},
    };
    document.sequence.tracks[0].clips[0].link_group_id =
        "01K30000000000000000000005";
    document.sequence.tracks[0].clips[1].link_group_id =
        "01K30000000000000000000005";
    return document;
}

// In-memory McpBackend for testing: no project file, no ProjectStorage.
// ApplyOperation/Undo/Redo call EditLog::Apply/Undo/Redo directly against a
// Document held in memory -- the same function ApplyOperationCommand's
// `--apply-op` path calls.
class InMemoryBackend : public McpBackend {
public:
    explicit InMemoryBackend(Document document,
                             bool applyProjectOperations = false)
        : project_(Project::FromDocument(std::move(document))),
          apply_project_operations_(applyProjectOperations) {
        document_ = project_.MakeActiveDocument();
    }

    // Cache artifacts the real backends read from the project package. Held
    // here so the tools that consume them can be exercised without a
    // ProjectStorage dependency.
    void SetSourceTranscript(Transcript transcript) {
        transcript_ = std::move(transcript);
    }

    // G7, G3 -- what each media's file says of its colour, as ingest would
    // read it again. A media absent here is a file that cannot be read.
    std::map<Ulid, MediaFileColor> file_colors;
    bool ReadMediaFileColor(const Ulid& mediaId, MediaFileColor& color,
                            std::string& message) override {
        const auto found = file_colors.find(mediaId);
        if (found == file_colors.end()) {
            message = "no file for media '" + mediaId + "'";
            return false;
        }
        color = found->second;
        return true;
    }
    void SetSourceShotQuality(ShotQualityReport report) {
        shot_quality_ = std::move(report);
    }
    void SetSourceSpeechOnset(SpeechOnsetReport report) {
        speech_onset_ = std::move(report);
    }
    void SetTimelineTranscriptHallucinated(bool value) {
        timeline_transcript_hallucinated_ = value;
    }
    void SetSourceHesitations(HesitationReport report) {
        hesitations_ = std::move(report);
        has_hesitations_ = true;
    }

    bool ReadSourceHesitations(const Ulid&, HesitationReport& report,
                               std::string& message) override {
        if (!has_hesitations_) {
            message = "no hesitation analysis in this fixture";
            return false;
        }
        report = hesitations_;
        return true;
    }
    HesitationReport hesitations_;
    bool has_hesitations_ = false;

    SpeechOnsetSettings speech_onset_settings;
    bool speech_onset_analysis_seen = false;

    bool AnalyzeSourceSpeechOnset(const Ulid&,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string&) override {
        speech_onset_settings = settings;
        speech_onset_analysis_seen = true;
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool ReadSourceSpeechOnset(const Ulid&, SpeechOnsetReport& report,
                               std::string& message) override {
        if (speech_onset_.levels.empty()) {
            message = "no speech envelope in this fixture";
            return false;
        }
        report = speech_onset_;
        return true;
    }

    bool ReadSourceTranscript(const Ulid&, Transcript& transcript,
                              std::string& message) override {
        if (transcript_.words.empty()) {
            message = "no transcript in this fixture";
            return false;
        }
        transcript = transcript_;
        return true;
    }

    // Records what the tool asked for, so the test can check the arguments
    // reach the engine rather than being swallowed by the dispatcher.
    struct TranscriptionRequest {
        std::vector<Ulid> media_ids;
        std::string language;
        bool verbatim = false;
        bool include_silent = false;
        bool seen = false;
        // PRD40 -- the call asked to replace a cached transcript regardless.
        bool overwrite = false;
    };
    TranscriptionRequest transcription_request;

    struct TimelineTranscriptionRequest {
        std::string timeline_id;
        std::string language;
        bool verbatim = false;
        bool seen = false;
    };
    TimelineTranscriptionRequest timeline_transcription_request;

    bool TranscribeSources(const std::vector<Ulid>& mediaIds,
                           const std::string& language, bool verbatim,
                           bool includeSilent, bool overwrite,
                           std::string& resultJson,
                           std::string& message) override {
        transcription_request = {mediaIds,      language, verbatim,
                                 includeSilent, true,     overwrite};
        if (transcription_fails_) {
            message = "no Whisper model configured";
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool TranscribeTimeline(const std::string& timelineId,
                            const std::string& language, bool verbatim,
                            std::string& resultJson, std::string&) override {
        timeline_transcription_request = {timelineId, language, verbatim, true};
        resultJson = "{\"ok\":true}";
        return true;
    }

    void FailTranscription() { transcription_fails_ = true; }
    void RequireExplicitTimeline() { require_explicit_timeline_ = true; }
    // PRD17 -- the catalog reads the mode from its backend.
    bool RequiresExplicitTimeline() const override {
        return require_explicit_timeline_;
    }
    void SetRequireExplicitTimeline(bool required) override {
        require_explicit_timeline_ = required;
    }

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override {
        if (require_explicit_timeline_ && timelineId.empty()) {
            errorName = "TimelineRequired";
            message =
                "strict timeline editing requires an explicit "
                "timeline_id";
            return false;
        }
        return McpBackend::SelectTimelineForEdit(timelineId, errorName,
                                                 message);
    }

    // QC-2026-09 (A1) -- records whether the alignment pass was asked to
    // write, because that boolean is the whole difference between a report
    // and a transcript the word-level tools can be trusted on.
    struct AlignmentRequest {
        bool apply = false;
        bool seen = false;
    };
    AlignmentRequest alignment_request;

    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string&) override {
        alignment_request = {apply, true};
        resultJson = "{\"ok\":true,\"applied\":";
        resultJson += apply ? "true" : "false";
        resultJson += ",\"sources\":[]}";
        return true;
    }

    ShotQualityCacheRead ReadSourceShotQuality(
        const Ulid&, ShotQualityReport& report) override {
        if (shot_quality_.samples.empty()) {
            return {ShotQualityCacheState::Absent, 0,
                    "no shot quality report in this fixture"};
        }
        report = shot_quality_;
        return {ShotQualityCacheState::Loaded, 4, {}};
    }

    bool SnapshotDocument(Document& document, std::string&) override {
        document = document_;
        return true;
    }

    bool CaptureTimelineSheet(const TimelineSheetPlan& plan,
                              const TimelineSheetSettings& settings,
                              std::string& jpegBytes, std::string&) override {
        captured_sheet_ = plan;
        captured_sheet_graded_ = settings.graded;
        jpegBytes = "fake-jpeg";
        return true;
    }

    // G1 -- the plan the tool built, and one measurement per clip, so the
    // reply's shape is checked without decoding a file.
    bool MeasureClipColors(const ColorMeasurePlan& plan,
                           std::vector<ColorClipMeasurement>& results,
                           std::string&) override {
        measured_plan_ = plan;
        results.clear();
        for (const Ulid& clipId : plan.clip_ids) {
            ColorClipMeasurement result;
            result.clip_id = clipId;
            result.source_time = {12, 25};
            result.width = 4;
            result.height = 2;
            result.picture.pixels = 8;
            result.picture.luma_p50 = 4150;
            result.region.pixels = 2;
            // AGENT-2026-09 A16 -- a scene median, when a test asks for
            // one, so propose_exposure has something to propose on.
            if (measured_scene_p50_) {
                result.picture.scene = true;
                result.picture.scene_p50 = *measured_scene_p50_;
            }
            results.push_back(result);
        }
        return true;
    }
    const std::optional<ColorMeasurePlan>& MeasuredPlan() const {
        return measured_plan_;
    }
    void SetMeasuredScene(int64_t p50Hundredths) {
        measured_scene_p50_ = p50Hundredths;
    }
    // G2 -- the plan match_color built, and a correction of one exposure
    // stage, so the reply's step is checked without decoding a file.
    bool MatchClipColor(const ColorMatchPlan& plan, ColorMatchResult& result,
                        std::string&) override {
        matched_plan_ = plan;
        result = ColorMatchResult{};
        result.grading_space = "display";
        result.knobs_after.exposure = 0.3;
        ClipEffect exposure;
        exposure.type = "color.exposure";
        exposure.params["amount"] = {3, 10};
        result.effects = {exposure};
        result.placements = {{"color.exposure", false}};
        return true;
    }
    const std::optional<ColorMatchPlan>& MatchedPlan() const {
        return matched_plan_;
    }
    // AGENT-2026-09 A20 -- the plan propose_grade built, and one proposal
    // per clip: a face found on the first, none on the others.
    bool ProposeClipGrades(const GradeProposalPlan& plan,
                           std::vector<GradeProposal>& proposals,
                           std::string&) override {
        graded_plan_ = plan;
        proposals.clear();
        for (size_t index = 0; index < plan.clip_ids.size(); ++index) {
            GradeProposal proposal;
            proposal.clip_id = plan.clip_ids[index];
            if (index == 0 || plan.anchor != "face") {
                proposal.anchor_used = plan.anchor;
                proposal.reason = "proposed";
                proposal.correction_hundredths = 45;
                proposal.knobs_after.exposure = 0.45;
                ClipEffect exposure;
                exposure.type = "color.exposure";
                exposure.params["amount"] = {45, 100};
                proposal.effects = {exposure};
                proposal.placements = {{"color.exposure", false}};
                proposal.balanced = plan.balance;
            } else {
                proposal.anchor_used = "none";
                proposal.reason = "no_face";
            }
            proposals.push_back(proposal);
        }
        return true;
    }
    const std::optional<GradeProposalPlan>& GradedPlan() const {
        return graded_plan_;
    }

    // Forwards the engine's refusal message, as the project backend does, so
    // a test can check what a refusal says and not only its code.
    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        EditError error = EditError::None;
        // MOTIF2 -- the call's motif rides on the entry, as on every
        // backend.
        std::vector<EntryMotif> motifs = EntryMotifsForCall(operation);
        if (!log_.Apply(document_, std::move(operation), error, message,
                        std::move(motifs))) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Undo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Undo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Redo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Redo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    // PL1 -- the same engine calls McpLiveBackend makes, without its check
    // for hand edits: nobody else edits this document.
    bool BeginIntervention(std::string& resultJson, std::string&,
                           std::string&) override {
        resultJson = InterventionBeganJson(
            MarkIntervention(document_.sequence.id, log_), document_);
        return true;
    }

    bool EndIntervention(const std::string& intervention,
                         const std::string& label, std::string& resultJson,
                         std::string& errorName,
                         std::string& message) override {
        InterventionMark mark;
        if (!ParseInterventionMark(intervention, mark, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        size_t grouped = 0;
        EditError error = EditError::None;
        if (!log_.GroupSince(document_, mark, label, grouped, error, message,
                             CurrentEditMotifs().motif)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = InterventionEndedJson(grouped, log_, document_);
        return true;
    }

    // PRD35 -- the journal this backend keeps, as describe's history reads.
    bool ReadTimelineHistory(TimelineHistory& history, std::string&,
                             std::string&) override {
        history = SummarizeHistory(document_.sequence.id, log_);
        return true;
    }

    // PA7 -- the movement a test hands in for a clip, where the real
    // backends decode the rush.
    std::map<Ulid, std::vector<StabilizationSample>> clip_motion;
    bool SampleClipMotion(const Ulid& clipId,
                          std::vector<StabilizationSample>& motion,
                          std::string& message) override {
        const auto found = clip_motion.find(clipId);
        if (found == clip_motion.end()) {
            message = "this backend cannot read a clip's movement";
            return false;
        }
        motion = found->second;
        return true;
    }

    // PA9 -- the same journal, as what changed since a state.
    bool ReadTimelineChangesJson(const std::string& sinceHash,
                                 std::string& json, std::string& errorName,
                                 std::string& message) override {
        TimelineChanges changes;
        if (!ReadTimelineChanges(document_, log_, sinceHash, changes,
                                 message)) {
            errorName = "InvalidDocument";
            return false;
        }
        json = TimelineChangesJson(changes);
        return true;
    }

    // MOTIF4 -- the same journal, grouped by motif.
    bool ReadTimelineMotifs(const std::optional<MotifReason>& reason,
                            const std::string& rule, std::string& json,
                            std::string&, std::string&) override {
        json = ListMotifsJson(document_.sequence.id, log_, reason, rule);
        return true;
    }

    bool Describe(std::string& json, std::string&) override {
        json = DescribeProject(project_);
        return true;
    }

    // S1 and S2 butt together; S3 sits after a gap. That shape is what lets
    // the tests below tell a legitimate merge from one that would silently
    // swallow a silence.
    bool ReadTimelineTranscript(std::string& json, std::string&) override {
        const std::string flag =
            timeline_transcript_hallucinated_ ? "true" : "false";
        json = R"({"timeline_id":"fixture","spans":[)";
        json +=
            R"({"span_id":"S1","source_id":"01K30000000000000000000001","source_in":{"value":100,"rate":25},"duration":{"value":10,"rate":25},"timeline_in":{"value":0,"rate":25},"text":"A strong hook.","likely_hallucinated":)" +
            flag + "},";
        json +=
            R"({"span_id":"S2","source_id":"01K30000000000000000000001","source_in":{"value":110,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":10,"rate":25},"text":"It keeps going.","likely_hallucinated":)" +
            flag + "},";
        json +=
            R"({"span_id":"S3","source_id":"01K30000000000000000000001","source_in":{"value":200,"rate":25},"duration":{"value":12,"rate":25},"timeline_in":{"value":25,"rate":25},"text":"And the payoff.","likely_hallucinated":)" +
            flag + "}]}";
        return true;
    }

    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override {
        if (!apply_project_operations_) {
            project_operation_ = std::move(operation);
            resultJson = "{\"ok\":true}";
            return true;
        }
        EditError error = EditError::None;
        if (!project_log_.Apply(project_, std::move(operation), error,
                                message)) {
            errorName = EditErrorName(error);
            return false;
        }
        document_ = project_.MakeActiveDocument();
        project_operation_ = project_log_.AppliedEntries().back().op;
        resultJson = "{\"ok\":true}";
        return true;
    }

    const Document& CurrentDocument() const { return document_; }
    EditLog& Log() { return log_; }
    const std::optional<TimelineSheetPlan>& CapturedSheet() const {
        return captured_sheet_;
    }
    bool CapturedSheetGraded() const { return captured_sheet_graded_; }
    const ProjectOperation* LastProjectOperation() const {
        return project_operation_ ? &*project_operation_ : nullptr;
    }

private:
    bool transcription_fails_ = false;
    bool require_explicit_timeline_ = false;
    bool apply_project_operations_ = false;
    bool timeline_transcript_hallucinated_ = false;
    Document document_;
    EditLog log_;
    std::optional<ProjectOperation> project_operation_;
    Project project_;
    ProjectEditLog project_log_;
    Transcript transcript_;
    ShotQualityReport shot_quality_;
    SpeechOnsetReport speech_onset_;
    std::optional<TimelineSheetPlan> captured_sheet_;
    bool captured_sheet_graded_ = false;
    std::optional<ColorMeasurePlan> measured_plan_;
    std::optional<int64_t> measured_scene_p50_;
    std::optional<ColorMatchPlan> matched_plan_;
    std::optional<GradeProposalPlan> graded_plan_;
};

// Minimal blocking HTTP client: sends one POST and reads the response until
// the peer closes the connection (HttpServer always responds with
// `Connection: close`).
std::string HttpPostJson(int port, const std::string& path,
                         const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string requestText = request.str();
    size_t sent = 0;
    while (sent < requestText.size()) {
        const ssize_t wrote =
            ::send(fd, requestText.data() + sent, requestText.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<size_t>(wrote);
    }
    std::string response;
    char chunk[4096];
    ssize_t got;
    while ((got = ::recv(fd, chunk, sizeof(chunk), 0)) > 0)
        response.append(chunk, static_cast<size_t>(got));
    ::close(fd);
    return response;
}

std::string StatusLine(const std::string& httpResponse) {
    return httpResponse.substr(0, httpResponse.find("\r\n"));
}

std::string HttpBody(const std::string& httpResponse) {
    const size_t headerEnd = httpResponse.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return "";
    return httpResponse.substr(headerEnd + 4);
}

// ---------------------------------------------------------------------
// PRD14 -- ROADMAP.md. A catalog that takes what an agent writes: a rush
// under either of its names, an enumeration in any case, a track by its
// alias, and refusals of edit_source_range that say what to send.
// ---------------------------------------------------------------------

mcp_json::Value ParsedJson(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    Check(mcp_json::Value::Parse(text, value, error),
          "test JSON parses: " + error + " in " + text);
    return value;
}

const char kFirstRush[] = "01K3T000000000000000000001";
const char kFirstVideoTrack[] = "01K3T000000000000000000002";
const char kFirstAudioTrack[] = "01K3T000000000000000000003";

// What a project holds before its first clip: one rush, which is both a
// library media (alias M1) and the montable source of the same ULID, as
// ingest makes them, and the two empty tracks create_project starts with.
Document FirstClipFixture() {
    Document document;
    LibraryMedia media;
    media.id = kFirstRush;
    media.path = "rushes/rush.mp4";
    media.filename = "rush.mp4";
    media.codec = "h264";
    media.width = 1280;
    media.height = 720;
    media.rate = {25, 1};
    media.duration = {150, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{media.id, media.path, media.rate, media.duration}};
    document.sequence.width = 1280;
    document.sequence.height = 720;
    document.sequence.frame_rate = {25, 1};
    document.sequence.tracks = {{kFirstVideoTrack, "video", 0, {}},
                                {kFirstAudioTrack, "audio", 1, {}}};
    return document;
}

// One place of a tool's schema that describes a value: a property, or the
// items of an array ("[]"). `parent` is the schema whose `properties` or
// `items` it was found in.
struct SchemaSite {
    std::vector<std::string> path;
    const mcp_json::Value* schema = nullptr;
    const mcp_json::Value* parent = nullptr;
    // Reached through oneOf, anyOf or allOf, which McpArgumentRules does
    // not enter.
    bool under_alternative = false;
};

void CollectSchemaSites(const mcp_json::Value& schema,
                        std::vector<std::string>& path, bool underAlternative,
                        std::vector<SchemaSite>& sites) {
    if (!schema.IsObject()) return;
    if (const mcp_json::Value* properties = schema.Find("properties");
        properties != nullptr && properties->IsObject()) {
        for (const auto& [name, child] : properties->AsObject()) {
            path.push_back(name);
            sites.push_back({path, &child, &schema, underAlternative});
            CollectSchemaSites(child, path, underAlternative, sites);
            path.pop_back();
        }
    }
    if (const mcp_json::Value* items = schema.Find("items")) {
        path.push_back("[]");
        sites.push_back({path, items, &schema, underAlternative});
        CollectSchemaSites(*items, path, underAlternative, sites);
        path.pop_back();
    }
    for (const char* key : {"oneOf", "anyOf", "allOf"}) {
        const mcp_json::Value* alternatives = schema.Find(key);
        if (alternatives == nullptr || !alternatives->IsArray()) continue;
        for (const mcp_json::Value& alternative : alternatives->AsArray()) {
            sites.push_back({path, &alternative, &schema, true});
            CollectSchemaSites(alternative, path, true, sites);
        }
    }
}

std::string SitePath(const std::string& tool, const SchemaSite& site) {
    std::string text = tool;
    for (const std::string& step : site.path)
        text += step == "[]" ? step : "." + step;
    return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

// The spellings of a schema that declares an enumeration of strings.
std::vector<std::string> EnumSpellings(const mcp_json::Value& schema) {
    std::vector<std::string> spellings;
    const mcp_json::Value* values = schema.Find("enum");
    if (values == nullptr || !values->IsArray()) return spellings;
    for (const mcp_json::Value& value : values->AsArray()) {
        if (!value.IsString()) return {};
        spellings.push_back(value.AsString());
    }
    return spellings;
}

// The same word in another case: upper when it has a lower-case letter,
// lower otherwise.
std::string OtherCase(const std::string& text) {
    const bool hasLower = std::any_of(
        text.begin(), text.end(),
        [](unsigned char character) { return std::islower(character) != 0; });
    std::string other = text;
    std::transform(other.begin(), other.end(), other.begin(),
                   [&](unsigned char character) {
                       return static_cast<char>(hasLower
                                                    ? std::toupper(character)
                                                    : std::tolower(character));
                   });
    return other;
}

// Arguments good enough to get past a dispatcher's first checks, built from
// the schema alone: every required property, with the fixture's own IDs
// where a name asks for one. They make the differential checks below reach
// the code that reads the argument under test, instead of stopping at the
// first "is required".
mcp_json::Value SampleObject(const mcp_json::Value& schema);

mcp_json::Value SampleValue(const mcp_json::Value& schema,
                            const std::string& name) {
    using mcp_json::Value;
    if (const std::vector<std::string> spellings = EnumSpellings(schema);
        !spellings.empty())
        return Value::MakeString(spellings.front());
    const Value* type = schema.Find("type");
    const std::string kind =
        type != nullptr && type->IsString() ? type->AsString() : std::string();
    if (kind == "string") {
        if (name.find("media_id") != std::string::npos ||
            name.find("source_id") != std::string::npos)
            return Value::MakeString("01K30000000000000000000001");
        if (name.find("audio_track_id") != std::string::npos)
            return Value::MakeString("01K30000000000000000000005");
        if (name.find("track_id") != std::string::npos)
            return Value::MakeString("01K30000000000000000000002");
        if (name.find("clip_id") != std::string::npos)
            return Value::MakeString("01K30000000000000000000003");
        if (name == "link_group_id")
            return Value::MakeString("01K30000000000000000000005");
        if (name.find("path") != std::string::npos || name == "output")
            return Value::MakeString("/nonexistent-prd14/x.mp4");
        return Value::MakeString("x");
    }
    if (kind == "integer" || kind == "number") return Value::MakeInt(1);
    if (kind == "boolean") return Value::MakeBool(false);
    if (kind == "array") {
        Value array = Value::MakeArray();
        if (const Value* items = schema.Find("items"))
            array.Push(SampleValue(*items, name));
        return array;
    }
    if (kind == "object") return SampleObject(schema);
    return Value::MakeString("x");
}

mcp_json::Value SampleObject(const mcp_json::Value& schema) {
    mcp_json::Value object = mcp_json::Value::MakeObject();
    const mcp_json::Value* properties = schema.Find("properties");
    const mcp_json::Value* required = schema.Find("required");
    if (properties == nullptr || required == nullptr || !required->IsArray())
        return object;
    for (const mcp_json::Value& name : required->AsArray()) {
        if (!name.IsString()) continue;
        if (const mcp_json::Value* child = properties->Find(name.AsString()))
            object.Set(name.AsString(), SampleValue(*child, name.AsString()));
    }
    return object;
}

// The sampled arguments of `schema` with `leaf` at `path`, its last property
// spelled `leafName` -- the declared name, or the other name of a rush.
mcp_json::Value PlaceAt(const mcp_json::Value& schema,
                        const std::vector<std::string>& path, size_t depth,
                        const std::string& leafName,
                        const mcp_json::Value& leaf) {
    using mcp_json::Value;
    if (depth == path.size()) return leaf;
    const std::string& step = path[depth];
    if (step == "[]") {
        Value array = Value::MakeArray();
        array.Push(
            PlaceAt(*schema.Find("items"), path, depth + 1, leafName, leaf));
        return array;
    }
    const bool last = depth + 1 == path.size();
    const Value sampled = SampleObject(schema);
    Value object = Value::MakeObject();
    for (const auto& [key, value] : sampled.AsObject())
        if (key != step) object.Set(key, value);
    object.Set(last ? leafName : step,
               PlaceAt(*schema.Find("properties")->Find(step), path, depth + 1,
                       leafName, leaf));
    return object;
}

// What sits at `path` in `arguments`, or nullptr.
const mcp_json::Value* ValueAt(const mcp_json::Value& arguments,
                               const std::vector<std::string>& path) {
    const mcp_json::Value* value = &arguments;
    for (const std::string& step : path) {
        if (value == nullptr) return nullptr;
        if (step == "[]") {
            value = value->IsArray() && !value->AsArray().empty()
                        ? &value->AsArray().front()
                        : nullptr;
        } else {
            value = value->Find(step);
        }
    }
    return value;
}

void CheckArgumentRules() {
    using mcp_json::Value;
    const std::string schema =
        R"({"type":"object","properties":{)"
        R"("source_id":{"type":"string","description":"Source rush."},)"
        R"("mode":{"type":"string","enum":["Insert","Overwrite"]},)"
        R"("kinds":{"type":"array","items":{"type":"string",)"
        R"("enum":["video","audio"]}},)"
        R"("trims":{"type":"array","items":{"type":"object","properties":{)"
        R"("edge":{"type":"string","enum":["Head","Tail"]},)"
        R"("media_ids":{"type":"array","items":{"type":"string"}}}}},)"
        R"("beat":{"type":"object","properties":{)"
        R"("media_id":{"type":"string"}}},)"
        R"("label":{"type":"string"}},"required":["source_id"]})";
    const McpArgumentRules rules(schema);
    Check(!rules.Empty(), "a schema with a rush and enumerations has rules");
    const auto normalize = [&](const std::string& json, std::string& message) {
        Value normalized;
        const bool accepted =
            rules.Normalize("tool", ParsedJson(json), normalized, message);
        return accepted ? normalized.Dump() : std::string("refused");
    };
    std::string message;
    Check(
        normalize(R"({"mode":"insert"})", message) == R"({"mode":"Insert"})" &&
            normalize(R"({"mode":"OVERWRITE"})", message) ==
                R"({"mode":"Overwrite"})" &&
            normalize(R"({"mode":"Insert"})", message) ==
                R"({"mode":"Insert"})",
        "an enumeration value is matched in any case and handed on in the "
        "schema's spelling");
    Check(normalize(R"({"mode":"replace"})", message) ==
                  R"({"mode":"replace"})" &&
              normalize(R"({"mode":"in_sert"})", message) ==
                  R"({"mode":"in_sert"})" &&
              normalize(R"({"mode":3})", message) == R"({"mode":3})",
          "a value that is no spelling of the enumeration is left for the "
          "tool to refuse: case is the only latitude");
    Check(normalize(R"({"kinds":["Video","AUDIO","caption"]})", message) ==
              R"({"kinds":["video","audio","caption"]})",
          "the items of an array are matched one by one");
    Check(
        normalize(R"({"trims":[{"edge":"head"},{"edge":"TAIL"}]})", message) ==
            R"({"trims":[{"edge":"Head"},{"edge":"Tail"}]})",
        "an enumeration inside an array of objects is reached");
    Check(normalize(R"({"media_id":"M1","mode":"insert","label":"x"})",
                    message) ==
              R"({"source_id":"M1","mode":"Insert","label":"x"})",
          "a rush given under its other name reaches the tool under the "
          "declared one, in the caller's order");
    Check(normalize(R"({"trims":[{"source_ids":["M1","M2"]}]})", message) ==
                  R"({"trims":[{"media_ids":["M1","M2"]}]})" &&
              normalize(R"({"beat":{"source_id":"M1"}})", message) ==
                  R"({"beat":{"media_id":"M1"}})",
          "the list of rushes and a nested rush take their other name too");
    Check(normalize(R"({"source_id":"M1","media_id":"M1"})", message) ==
              R"({"source_id":"M1"})",
          "the same value under both names is one argument");
    message.clear();
    Check(normalize(R"({"source_id":"M1","media_id":"M2"})", message) ==
                  "refused" &&
              message.find("'source_id'") != std::string::npos &&
              message.find("'media_id'") != std::string::npos &&
              message.find("different values") != std::string::npos,
          "two values under the two names of a rush are refused by name: " +
              message);
    message.clear();
    Check(normalize(R"({"trims":[{},{"media_ids":["M1"],"source_ids":[]}]})",
                    message) == "refused" &&
              message.find("'tool.trims[1]'") != std::string::npos,
          "the refusal says where the two names met: " + message);
    Check(normalize(R"({"other":{"mode":"insert"},"media_ids":["M1"]})",
                    message) ==
              R"({"other":{"mode":"insert"},"media_ids":["M1"]})",
          "what the schema does not declare is passed on untouched, for the "
          "tool to refuse");
    {
        Value once;
        Value twice;
        std::string ignored;
        const Value given = ParsedJson(
            R"({"media_id":"M1","mode":"insert","kinds":["VIDEO"]})");
        Check(rules.Normalize("tool", given, once, ignored) &&
                  rules.Normalize("tool", once, twice, ignored) &&
                  once.Dump() == twice.Dump(),
              "normalising twice changes nothing more");
    }

    // Two spellings that differ only by case: an exact match wins, and a
    // third case is nobody's.
    const McpArgumentRules twins(
        R"({"type":"object","properties":{"unit":{"type":"string",)"
        R"("enum":["ms","MS","frames"]}}})");
    Value unit;
    Check(twins.Normalize("tool", ParsedJson(R"({"unit":"MS"})"), unit,
                          message) &&
              unit.Dump() == R"({"unit":"MS"})" &&
              twins.Normalize("tool", ParsedJson(R"({"unit":"Ms"})"), unit,
                              message) &&
              unit.Dump() == R"({"unit":"Ms"})" &&
              twins.Normalize("tool", ParsedJson(R"({"unit":"FRAMES"})"), unit,
                              message) &&
              unit.Dump() == R"({"unit":"frames"})",
          "an ambiguous case is not guessed");

    // A tool that declares both names means two arguments by them.
    const McpArgumentRules both(
        R"({"type":"object","properties":{"media_id":{"type":"string"},)"
        R"("source_id":{"type":"string"}}})");
    Check(both.Empty(),
          "a schema declaring both names of a rush has no rule about them");
    const McpArgumentRules none(
        R"({"type":"object","properties":{"clip_id":{"type":"string"}}})");
    Value untouched;
    Check(none.Empty() &&
              none.Normalize("tool", ParsedJson(R"({"media_id":"M1"})"),
                             untouched, message) &&
              untouched.Dump() == R"({"media_id":"M1"})",
          "a schema with no rush and no enumeration has no rule");
    Check(McpArgumentRules("not json").Empty(),
          "a schema that does not parse yields no rule");

    Check(std::string(RushArgumentTwin("media_id")) == "source_id" &&
              std::string(RushArgumentTwin("source_id")) == "media_id" &&
              std::string(RushArgumentTwin("media_ids")) == "source_ids" &&
              std::string(RushArgumentTwin("source_ids")) == "media_ids" &&
              RushArgumentTwin("clip_id") == nullptr,
          "a rush has two names, singular and plural");
}

// Every tool of the catalog, every argument its schema declares: what the
// schema says is what the catalog accepts. Driven by the catalog itself, so
// a tool added tomorrow is held to the same rules without a line here.
void CheckCatalogTolerance(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;

    // The fixture's rush as a library media too, so M1 exists and the sample
    // arguments name a real one.
    Document document = fixture;
    {
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = document.sources[0].path;
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = document.sources[0].rate;
        media.duration = document.sources[0].duration;
        media.orientation = "landscape";
        document.library = {media};
    }
    const std::string videoTrack = "01K30000000000000000000002";
    const std::string audioTrack = "01K30000000000000000000005";

    // Same call, same answer: `left` and `right` differ only by the
    // spelling under test.
    const auto sameAnswer = [&](const std::string& tool, const Value& left,
                                const Value& right, const std::string& label) {
        InMemoryBackend leftBackend(document);
        InMemoryBackend rightBackend(document);
        const McpToolCallOutcome expected =
            registry.Call(leftBackend, tool, left);
        const McpToolCallOutcome got = registry.Call(rightBackend, tool, right);
        Check(expected.ok == got.ok && expected.error_name == got.error_name &&
                  expected.message == got.message,
              label + ": " + right.Dump() + " answers '" + got.message +
                  "' where " + left.Dump() + " answers '" + expected.message +
                  "'");
        Check(got.message.find("unknown argument") == std::string::npos &&
                  got.message.find("unknown key") == std::string::npos &&
                  got.message.find("must be one of") == std::string::npos,
              label + " is refused for its spelling: " + got.message);
    };

    // Criterion 5 -- identifiers of a lot or a ticket are development
    // history, which an agent pays for in bytes on every load. The reason
    // stays in a comment beside the registration. A single letter followed
    // by one digit is left alone: it is an alias or an example (A1, M1, K1,
    // T1, B4) and cannot be told from a ticket.
    const std::regex lotName("[A-Z][A-Z-]*-20[0-9][0-9]-[0-9][0-9]");
    const std::regex ticketName(
        "\\b(PRD|AX|IA|PL|PERF|WF|VQA|REM|MB|MA|ITW|EP|BENCH|ACP|GR|QC)"
        "[0-9]+\\b|\\b[A-Z][0-9]{2,}\\b|\\b[A-Z][0-9]\\.[0-9]\\b|"
        "\\bROADMAP\\b");
    // A letter and digits that are not a ticket: a span id given as an
    // example, EBU R128, and the codecs by their usual names.
    const std::set<std::string> notTickets = {"S12", "R128", "H264", "H265"};
    const auto checkProse = [&](const std::string& where,
                                const std::string& text) {
        std::smatch match;
        Check(!std::regex_search(text, match, lotName),
              where + " names a lot of the roadmap: " + match.str());
        for (std::sregex_iterator found(text.begin(), text.end(), ticketName),
             end;
             found != end; ++found) {
            Check(notTickets.count(found->str()) != 0,
                  where + " names a ticket of the roadmap: " + found->str());
        }
    };

    // The tools that preview, and what leaving `preview` out does. A tool
    // that gains the argument is added here, with its default said in the
    // argument's description.
    const std::map<std::string, bool> previewDefaults = {
        {"edit_source_range", false},   {"delete_timeline_range", false},
        {"shorten_linked_clip", false}, {"generate_captions", false},
        {"translate_captions", false},  {"apply_cdl", false},
        {"conform_sequence", false},    {"conform_color_management", false},
        {"split_link_groups", false},   {"create_timeline_variants", true},
        {"export_timelines", true},     {"render_motion_video", true},
    };
    size_t previews = 0;
    size_t rushes = 0;
    size_t enumerations = 0;
    size_t capitalised = 0;
    size_t trackArguments = 0;

    for (const McpTool& tool : registry.Tools()) {
        Value schema;
        std::string parseError;
        Check(Value::Parse(tool.input_schema_json, schema, parseError),
              tool.name + " publishes a schema that parses: " + parseError);
        checkProse(tool.name, tool.description);

        std::vector<SchemaSite> sites;
        std::vector<std::string> path;
        CollectSchemaSites(schema, path, false, sites);
        if (const Value* description = schema.Find("description");
            description != nullptr && description->IsString())
            checkProse(tool.name + " arguments", description->AsString());

        for (const SchemaSite& site : sites) {
            const std::string where = SitePath(tool.name, site);
            const std::string name = site.path.empty() ? "" : site.path.back();
            const Value* description = site.schema->Find("description");
            const std::string text =
                description != nullptr && description->IsString()
                    ? description->AsString()
                    : std::string();
            checkProse(where, text);

            const std::vector<std::string> spellings =
                EnumSpellings(*site.schema);
            const Value* properties = site.parent->Find("properties");
            const char* twin = name == "[]" || site.under_alternative
                                   ? nullptr
                                   : RushArgumentTwin(name);
            const bool rush = twin != nullptr && properties != nullptr &&
                              properties->Find(twin) == nullptr;
            if (site.under_alternative) {
                Check(spellings.empty() && RushArgumentTwin(name) == nullptr,
                      where +
                          " declares a rush or an enumeration under an "
                          "alternative, where McpArgumentRules does not "
                          "look");
                continue;
            }

            // Criterion 6 -- every preview says what leaving it out does.
            if (name == "preview" && site.path.size() == 1) {
                ++previews;
                const auto expected = previewDefaults.find(tool.name);
                Check(expected != previewDefaults.end(),
                      tool.name +
                          " previews: say its default in "
                          "previewDefaults and in its description");
                if (expected != previewDefaults.end()) {
                    Check(text.find(expected->second
                                        ? "default true"
                                        : "default false") != std::string::npos,
                          where + " says its default: " + text);
                }
            }

            // Criterion 1 -- a rush takes its other name. The schema no
            // longer says so on each argument: the call reaches the tool the
            // same way under either name.
            if (rush) {
                ++rushes;
                Check(text.find("Also accepted as") == std::string::npos,
                      where + " no longer spells its other name: " + text);
                const Value sample = SampleValue(*site.schema, name);
                const Value declared =
                    PlaceAt(schema, site.path, 0, name, sample);
                const Value other = PlaceAt(schema, site.path, 0, twin, sample);
                Value normalized;
                std::string refusal;
                Check(registry.NormalizeArguments(tool.name, other, normalized,
                                                  refusal) &&
                          normalized.Dump() == declared.Dump(),
                      where + " given as " + twin + " reaches the tool as " +
                          name + ": " + normalized.Dump() + " " + refusal);
                sameAnswer(tool.name, declared, other, where + " as " + twin);
            }

            // Criterion 2 -- an enumeration takes any case.
            if (!spellings.empty()) {
                ++enumerations;
                if (std::any_of(spellings.begin(), spellings.end(),
                                [](const std::string& spelling) {
                                    return std::any_of(
                                        spelling.begin(), spelling.end(),
                                        [](unsigned char character) {
                                            return std::isupper(character) != 0;
                                        });
                                }))
                    ++capitalised;
                for (const std::string& spelling : spellings) {
                    const std::string cased = OtherCase(spelling);
                    if (cased == spelling) continue;
                    const Value declared = PlaceAt(schema, site.path, 0, name,
                                                   Value::MakeString(spelling));
                    const Value other = PlaceAt(schema, site.path, 0, name,
                                                Value::MakeString(cased));
                    Value normalized;
                    std::string refusal;
                    const Value* reached =
                        registry.NormalizeArguments(tool.name, other,
                                                    normalized, refusal)
                            ? ValueAt(normalized, site.path)
                            : nullptr;
                    Check(reached != nullptr && reached->IsString() &&
                              reached->AsString() == spelling,
                          where + " given as '" + cased +
                              "' reaches the tool as '" + spelling + "'");
                    sameAnswer(tool.name, declared, other,
                               where + " as '" + cased + "'");
                }
            }

            // Criterion 3 -- every argument that names a track takes its
            // alias: T1 is the fixture's video track, T2 its audio track.
            const bool trackList = EndsWith(name, "track_ids");
            if (name == "track_id" || EndsWith(name, "_track_id") ||
                trackList) {
                ++trackArguments;
                const bool audio = name.find("audio") != std::string::npos;
                const auto named = [&](const std::string& value) {
                    if (!trackList) return Value::MakeString(value);
                    Value list = Value::MakeArray();
                    list.Push(Value::MakeString(value));
                    return list;
                };
                sameAnswer(tool.name,
                           PlaceAt(schema, site.path, 0, name,
                                   named(audio ? audioTrack : videoTrack)),
                           PlaceAt(schema, site.path, 0, name,
                                   named(audio ? "T2" : "t1")),
                           where + " by alias");
            }
        }
    }
    // The counts of the finding, so the loop above cannot pass by having
    // found nothing to check.
    Check(previews == previewDefaults.size(),
          "every tool listed as previewing has a preview argument: " +
              std::to_string(previews));
    Check(rushes >= 29, "the catalog's rush arguments were all checked: " +
                            std::to_string(rushes));
    Check(enumerations >= 42 && capitalised >= 14,
          "the catalog's enumerations were all checked: " +
              std::to_string(enumerations) + ", " +
              std::to_string(capitalised) + " capitalised");
    Check(trackArguments >= 30,
          "the catalog's track arguments were all "
          "checked: " +
              std::to_string(trackArguments));

    // Four track arguments the sampled call does not reach: their tools
    // refuse it first, for want of a clip or of a second move. Found by
    // unregistering the track aliases, which the loop above caught on 26
    // arguments out of 30. Named here with arguments that do reach them;
    // the alias no track has (T9) proves the argument is read.
    const auto byAlias = [&](const std::string& tool, const std::string& json) {
        const auto with = [&](const std::string& track) {
            static const std::string kPlace = "%TRACK%";
            std::string text = json;
            for (size_t at = text.find(kPlace); at != std::string::npos;
                 at = text.find(kPlace))
                text.replace(at, kPlace.size(), track);
            return ParsedJson(text);
        };
        sameAnswer(tool, with(audioTrack), with("T2"),
                   tool + " track by alias");
        InMemoryBackend backend(document);
        const McpToolCallOutcome unknown =
            registry.Call(backend, tool, with("T9"));
        Check(unknown.message.find("no object matches id or prefix 'T9'") !=
                  std::string::npos,
              tool +
                  " reads its track argument on this call: " + unknown.message);
    };
    byAlias("tighten_pauses",
            R"({"clip_id":"A1","sync_track_ids":["%TRACK%"]})");
    byAlias("trim_boundary_air",
            R"({"clip_id":"A1","sync_track_ids":["%TRACK%"]})");
    byAlias("close_junction_air",
            R"({"left_clip_id":"A1","right_clip_id":"A2",)"
            R"("sync_track_ids":["%TRACK%"]})");
    byAlias("move_clips", R"({"moves":[{"clip_id":"A1","track_id":"%TRACK%",)"
                          R"("timeline_in":{"value":40,"rate":25}},)"
                          R"({"clip_id":"A2","track_id":"%TRACK%",)"
                          R"("timeline_in":{"value":60,"rate":25}}]})");
}

// The sequence of the finding, on a project that holds one rush and no clip:
// what took five calls and a ULID copied out of describe takes one.
void CheckFirstClip() {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const Document empty = FirstClipFixture();
    const auto call = [&](McpBackend& backend, const std::string& tool,
                          const std::string& json) {
        return registry.Call(backend, tool, ParsedJson(json));
    };

    // Criterion 3 -- the tracks have an alias, shown where the others are.
    {
        InMemoryBackend backend(empty);
        const McpToolCallOutcome described =
            call(backend, "describe", R"({"only":["timeline","library"]})");
        Check(
            described.ok &&
                described.result_json.find(
                    std::string(R"("tracks":[{"alias":"T1","id":")") +
                    kFirstVideoTrack + R"(","kind":"video")") !=
                    std::string::npos &&
                described.result_json.find(
                    std::string(R"({"alias":"T2","id":")") + kFirstAudioTrack +
                    R"(","kind":"audio")") != std::string::npos &&
                described.result_json.find(R"("library":[{"alias":"M1")") !=
                    std::string::npos,
            "describe names the tracks T1 and T2 beside the rush M1: " +
                described.result_json);
        const IdResolver resolver(empty);
        Ulid resolved;
        std::string error;
        Check(resolver.Resolve("track_id", "T1", resolved, error) &&
                  resolved == kFirstVideoTrack &&
                  resolver.Resolve("track_id", "t2", resolved, error) &&
                  resolved == kFirstAudioTrack,
              "the resolver takes a track alias, in either case: " + error);
        Check(!resolver.Resolve("track_id", "T3", resolved, error) &&
                  error.find("no object matches") != std::string::npos,
              "a track alias past the last track names nothing: " + error);
    }

    // The finding, call by call.
    {
        InMemoryBackend backend(empty);
        // 1 -- `media_id` was an unknown argument; the track was then
        // "required", with nothing to say which.
        const McpToolCallOutcome noTrack =
            call(backend, "edit_source_range",
                 R"({"media_id":"M1","mode":"insert"})");
        CheckFailureEnvelope(noTrack, "a Source edit with no track",
                             "ValidationFailed");
        Check(noTrack.message ==
                  std::string("'edit_source_range.track_id' is required: the "
                              "unlocked video tracks of this timeline are "
                              "T1 (") +
                      kFirstVideoTrack + ")",
              "a missing track is refused with the tracks that would do: " +
                  noTrack.message);
        // 2 and 3 -- the track by its alias, the mode in lower case; what
        // is missing is now where the range goes, and the refusal says in
        // which arguments and in which form.
        const McpToolCallOutcome noPoint =
            call(backend, "edit_source_range",
                 R"({"media_id":"M1","track_id":"T1","mode":"insert"})");
        CheckFailureEnvelope(noPoint, "a Source edit with no Record point",
                             "ValidationFailed");
        Check(noPoint.message.find("timeline_in or timeline_out") !=
                      std::string::npos &&
                  noPoint.message.find("source_in and source_out") !=
                      std::string::npos &&
                  noPoint.message.find("{value, rate}") != std::string::npos &&
                  noPoint.message.find(R"({"value":0,"rate":25})") !=
                      std::string::npos &&
                  std::count(noPoint.message.begin(), noPoint.message.end(),
                             '.') == 0,
              "missing points are refused with which to give and in what "
              "form, in one sentence: " +
                  noPoint.message);
        Check(backend.Log().AppliedCount() == 0,
              "neither refusal wrote anything");
        // 4 -- the call an agent writes: picture and linked sound of the
        // whole rush at the start of the timeline.
        const McpToolCallOutcome laid =
            call(backend, "edit_source_range",
                 R"({"media_id":"M1","track_id":"T1","audio_track_ids":["T2"],)"
                 R"("mode":"insert","timeline_in":{"value":0,"rate":25}})");
        const auto& tracks = backend.CurrentDocument().sequence.tracks;
        Check(laid.ok && backend.Log().AppliedCount() == 1 &&
                  tracks[0].clips.size() == 1 && tracks[1].clips.size() == 1 &&
                  tracks[0].clips[0].source_id == kFirstRush &&
                  tracks[0].clips[0].duration == RationalTime{150, 25} &&
                  tracks[0].clips[0].timeline_in == RationalTime{0, 25} &&
                  tracks[1].clips[0].link_group_id ==
                      tracks[0].clips[0].link_group_id &&
                  !tracks[0].clips[0].link_group_id.empty() &&
                  laid.result_json.find(R"("mode":"Insert")") !=
                      std::string::npos,
              "the first clip is laid in one call, by aliases, in the "
              "caller's own case: " +
                  laid.message + " " + laid.result_json);
    }

    // Criterion 1 -- the two names of a rush, beyond edit_source_range.
    {
        InMemoryBackend backend(empty);
        const McpToolCallOutcome both =
            call(backend, "edit_source_range",
                 std::string(R"({"source_id":"M1","media_id":")") + kFirstRush +
                     R"(","track_id":"T1","mode":"Insert",)"
                     R"("timeline_in":{"value":0,"rate":25}})");
        CheckFailureEnvelope(both, "a rush named twice with two values",
                             "ValidationFailed");
        Check(both.message.find("'source_id'") != std::string::npos &&
                  both.message.find("'media_id'") != std::string::npos,
              "the refusal names both arguments: " + both.message);
        Check(call(backend, "edit_source_range",
                   R"({"source_id":"M1","media_id":"M1","track_id":"T1",)"
                   R"("mode":"Insert","timeline_in":{"value":0,"rate":25}})")
                  .ok,
              "the same rush under both names is one argument");
        Check(call(backend, "transcribe_media", R"({"source_id":"M1"})").ok &&
                  backend.transcription_request.media_ids ==
                      std::vector<Ulid>{kFirstRush},
              "a tool that says media_id takes source_id");
        backend.transcription_request = {};
        Check(
            call(backend, "transcribe_media", R"({"source_ids":["M1"]})").ok &&
                backend.transcription_request.media_ids ==
                    std::vector<Ulid>{kFirstRush},
            "a tool that says media_ids takes source_ids");
        Check(call(backend, "insert_clip",
                   R"({"track_id":"T1","media_id":"M1",)"
                   R"("source_in":{"value":0,"rate":25},)"
                   R"("duration":{"value":25,"rate":25},)"
                   R"("timeline_in":{"value":0,"rate":25},)"
                   R"("sync_track_ids":["T2"]})")
                  .ok,
              "insert_clip takes media_id, and its tracks by alias");
        const McpToolCallOutcome pasted =
            call(backend, "paste_clips",
                 R"({"clips":[{"label":"a","track_id":"T1","media_id":"M1",)"
                 R"("source_in":{"value":0,"rate":25},)"
                 R"("duration":{"value":10,"rate":25},)"
                 R"("timeline_in":{"value":0,"rate":25}}],"overwrite":true})");
        Check(pasted.message.find("unknown") == std::string::npos,
              "a rush inside a list of clips takes its other name: " +
                  pasted.message);
    }

    // Criterion 2 -- case, on the enumerations an agent meets first.
    {
        InMemoryBackend backend(empty);
        Check(call(backend, "add_track", R"({"kind":"Video"})").ok &&
                  backend.CurrentDocument().sequence.tracks.size() == 3 &&
                  backend.CurrentDocument().sequence.tracks.back().kind ==
                      "video",
              "a lower-case enumeration takes a capital");
        const McpToolCallOutcome wrong =
            call(backend, "edit_source_range",
                 R"({"media_id":"M1","track_id":"T1","mode":"replace",)"
                 R"("timeline_in":{"value":0,"rate":25}})");
        Check(!wrong.ok && wrong.message ==
                               "'edit_source_range.mode' must be 'Insert' or "
                               "'Overwrite'",
              "a value that is not of the enumeration is still refused, in "
              "the canonical spelling: " +
                  wrong.message);
        const McpToolCallOutcome described = call(
            backend, "describe", R"({"only":["timeline"],"kinds":["AUDIO"]})");
        Check(described.ok &&
                  described.result_json.find(R"("kind":"audio")") !=
                      std::string::npos &&
                  described.result_json.find(R"("kind":"video")") ==
                      std::string::npos,
              "an enumeration inside a list takes any case: " +
                  described.message);
    }

    // A plan's steps reach the dispatchers without Call: they are read the
    // same way.
    {
        InMemoryBackend backend(empty);
        const McpToolCallOutcome prepared =
            call(backend, "prepare_edit_plan",
                 R"({"label":"Premier plan","steps":[{"tool":)"
                 R"("edit_source_range","arguments":{"media_id":"M1",)"
                 R"("track_id":"T1","mode":"insert",)"
                 R"("timeline_in":{"value":0,"rate":25}}}]})");
        Check(prepared.ok && backend.Log().AppliedCount() == 0,
              "a step of a plan takes the same names, case and aliases: " +
                  prepared.message);
    }

    // Criterion 4 -- a wrong target is refused by what is wrong with it.
    {
        Document locked = empty;
        locked.sequence.tracks.push_back(
            {"01K3T000000000000000000004", "video", 2, {}});
        locked.sequence.tracks.back().locked = true;
        InMemoryBackend backend(locked);
        const std::string offer =
            std::string(
                ": the unlocked video tracks of this timeline are "
                "T1 (") +
            kFirstVideoTrack + ")";
        const auto refusal = [&](const std::string& arguments) {
            const McpToolCallOutcome outcome =
                call(backend, "edit_source_range",
                     R"({"media_id":"M1","mode":"insert",)"
                     R"("timeline_in":{"value":0,"rate":25},)" +
                         arguments + "}");
            CheckFailureEnvelope(outcome, "a Source edit on " + arguments,
                                 "ValidationFailed");
            return outcome.message;
        };
        Check(refusal(R"("track_id":"T2")") ==
                  "'edit_source_range.track_id' names an audio track" + offer,
              "an audio target is named as one: " +
                  refusal(R"("track_id":"T2")"));
        Check(refusal(R"("track_id":"T3")") ==
                  "'edit_source_range.track_id' names a locked track" + offer,
              "a locked target is named as one: " +
                  refusal(R"("track_id":"T3")"));
        Check(refusal(R"("track_id":"M1")") ==
                  "'edit_source_range.track_id' names no track of this "
                  "timeline" +
                      offer,
              "something that is not a track is named as such: " +
                  refusal(R"("track_id":"M1")"));
        Check(refusal(R"("track_id":"T1","audio_track_ids":["T1"])") ==
                  std::string(
                      "'edit_source_range.audio_track_ids[0]' names a video "
                      "track: the unlocked audio tracks of this timeline "
                      "are T2 (") +
                      kFirstAudioTrack + ")",
              "an audio target that is not one is refused with those that "
              "are: " +
                  refusal(R"("track_id":"T1","audio_track_ids":["T1"])"));
        Check(refusal(R"("track_id":"T1","audio_track_ids":["T2","T2"])")
                      .find("audio_track_ids[1]' names a track already "
                            "listed") != std::string::npos,
              "an audio target named twice is refused as such");
        // The name an editor gives a track by habit resolves to nothing:
        // the refusal still says which tracks there are.
        Check(refusal(R"("track_id":"V1")") ==
                  "'edit_source_range.track_id': no object matches id or "
                  "prefix 'V1'" +
                      offer,
              "a track name that resolves to nothing is answered with the "
              "tracks: " +
                  refusal(R"("track_id":"V1")"));
        const McpToolCallOutcome noMode =
            call(backend, "edit_source_range",
                 R"({"media_id":"M1","track_id":"T1"})");
        Check(
            !noMode.ok && noMode.message ==
                              "'edit_source_range.mode' is required: "
                              "'Insert' or 'Overwrite'",
            "a missing mode is refused with its two values: " + noMode.message);
        Check(backend.Log().AppliedCount() == 0, "no refusal wrote anything");

        Document allLocked = empty;
        allLocked.sequence.tracks[0].locked = true;
        InMemoryBackend lockedBackend(allLocked);
        const McpToolCallOutcome nowhere =
            call(lockedBackend, "edit_source_range",
                 R"({"media_id":"M1","mode":"insert"})");
        Check(!nowhere.ok &&
                  nowhere.message.find("this timeline has no unlocked video "
                                       "track") != std::string::npos &&
                  nowhere.message.find("add_track") != std::string::npos,
              "a timeline with no target says so and how to get one: " +
                  nowhere.message);

        Document still = empty;
        still.sources[0].duration = {0, 25};
        still.library[0].duration = {0, 25};
        InMemoryBackend stillBackend(still);
        const McpToolCallOutcome unmeasured =
            call(stillBackend, "edit_source_range",
                 R"({"media_id":"M1","track_id":"T1","mode":"insert",)"
                 R"("timeline_in":{"value":0,"rate":25}})");
        Check(!unmeasured.ok &&
                  unmeasured.message.find("has no duration on record") !=
                      std::string::npos,
              "a rush with no duration is refused as that, not as a missing "
              "point: " +
                  unmeasured.message);
    }

    // Criterion 3 -- what a track alias is worth after tracks come and go.
    {
        InMemoryBackend backend(empty);
        Check(call(backend, "add_track", R"({"kind":"video"})").ok,
              "a track is added with no index");
        const auto trackOf = [&](const std::string& alias) {
            const IdResolver resolver(backend.CurrentDocument());
            Ulid resolved;
            std::string error;
            return resolver.Resolve("track_id", alias, resolved, error)
                       ? resolved
                       : std::string();
        };
        const Ulid added = backend.CurrentDocument().sequence.tracks.back().id;
        Check(trackOf("T1") == kFirstVideoTrack &&
                  trackOf("T2") == kFirstAudioTrack && trackOf("T3") == added,
              "a track added with no index takes the next number and moves "
              "no other");
        Check(call(backend, "set_track_lock",
                   R"({"track_id":"T3","locked":true})")
                      .ok &&
                  backend.CurrentDocument().sequence.tracks.back().locked,
              "a track tool takes the alias");
        Check(call(backend, "remove_track", R"({"track_id":"T1"})").ok &&
                  trackOf("T1") == kFirstAudioTrack && trackOf("T2") == added &&
                  trackOf("T3").empty(),
              "removing a track renumbers the tracks after it");
    }

    // The twentieth track's clips were T1, T2... before tracks had an alias:
    // they keep the name, and the tracks they would hide are named by ID.
    {
        Document wide = empty;
        wide.sequence.tracks.clear();
        for (int index = 0; index < 20; ++index) {
            wide.sequence.tracks.push_back(
                {GenerateUlid(), index == 0 ? "video" : "audio", index, {}});
        }
        wide.sequence.tracks[19].clips = {
            {GenerateUlid(), kFirstRush, {0, 25}, {10, 25}, {0, 25}},
            {GenerateUlid(), kFirstRush, {0, 25}, {10, 25}, {20, 25}}};
        const IdResolver resolver(wide);
        Ulid resolved;
        std::string error;
        Check(resolver.Resolve("clip_id", "T1", resolved, error) &&
                  resolved == wide.sequence.tracks[19].clips[0].id &&
                  resolver.Resolve("clip_id", "T2", resolved, error) &&
                  resolved == wide.sequence.tracks[19].clips[1].id,
              "a clip alias accepted before tracks had one keeps its "
              "meaning: " +
                  error);
        Check(resolver.Resolve("track_id", "T3", resolved, error) &&
                  resolved == wide.sequence.tracks[2].id &&
                  resolver.Resolve("track_id", "T20", resolved, error) &&
                  resolved == wide.sequence.tracks[19].id,
              "the tracks no clip hides keep their alias: " + error);
        const std::vector<const DocumentTrack*> listed =
            TracksInDescribeOrder(wide.sequence);
        Check(TrackAlias(listed, 0).empty() && TrackAlias(listed, 1).empty() &&
                  TrackAlias(listed, 2) == "T3",
              "a hidden track alias is not shown");
        const std::string described = DescribeDocument(wide);
        Check(
            described.find(R"("tracks":[{"id":")" + wide.sequence.tracks[0].id +
                           "\"") != std::string::npos &&
                described.find(R"({"alias":"T3","id":")" +
                               wide.sequence.tracks[2].id + "\"") !=
                    std::string::npos &&
                described.find(R"("alias":"T1","id":")" +
                               wide.sequence.tracks[19].clips[0].id + "\"") !=
                    std::string::npos,
            "describe shows only the aliases the tools resolve");
    }
}

// ---------------------------------------------------------------------
// PRD15 -- ROADMAP.md. Every tool says what it does to what lies around it,
// asks in `required` for what its code requires, and fits in what a client
// reads.
// ---------------------------------------------------------------------

// The number of characters (UTF-8 code points) in `text`: Claude Code cuts a
// description at 2,048 of them.
size_t CharacterCount(const std::string& text) {
    return static_cast<size_t>(
        std::count_if(text.begin(), text.end(), [](char byte) {
            return (static_cast<unsigned char>(byte) & 0xC0) != 0x80;
        }));
}

void CheckToolClassesAndAnnotations(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;

    // A: every tool is classified, and the table holds no name no tool has.
    std::set<std::string> toolNames;
    for (const McpTool& tool : registry.Tools()) {
        toolNames.insert(tool.name);
        Check(tool.tool_class != McpToolClass::Unclassified,
              tool.name + " declares its class in the registry's table");
    }
    const std::vector<std::string> tableNames = McpToolClassTableNames();
    const std::set<std::string> declared(tableNames.begin(), tableNames.end());
    Check(declared.size() == tableNames.size(),
          "the class table names each tool once");
    for (const std::string& name : tableNames)
        Check(toolNames.count(name) != 0,
              "the class table names a tool the catalog has: " + name);

    // A3: the class never contradicts the flags McpTool already carried.
    // propose_grade edits with apply:true and is still registered as a
    // timeline read, so it takes no idempotency_key: a gap of the registry
    // named here, not a class to bend.
    // OR2 -- commit_run_step has its own durable step_id. Session answer
    // caching would hide journal divergence after an intervening human edit.
    const std::set<std::string> editsWithoutKey = {"propose_grade",
                                                   "commit_run_step"};
    for (const McpTool& tool : registry.Tools()) {
        const McpToolClass c = tool.tool_class;
        const std::string name = tool.name + " (" + McpToolClassName(c) + ")";
        Check(tool.writes_analysis_cache == (c == McpToolClass::Analysis),
              name + ": an analysis is exactly a tool that writes a cache");
        // PRD16 -- or an export, which still writes the file its caller
        // named: running as a task does not change what it does.
        // SYNCHRO-2026-10 SY1 -- or a read that decodes rushes whole
        // (measure_audio_offset), which writes nothing at all.
        Check(!tool.background || c == McpToolClass::Analysis ||
                  c == McpToolClass::FileWrite || c == McpToolClass::Read,
              name +
                  ": what runs in the background is an analysis, an export "
                  "or a long read");
        // PRD40 -- or writes the file an export names, whose answer can be
        // lost like an edit's.
        Check(
            !tool.idempotent || c == McpToolClass::JournaledEdit ||
                c == McpToolClass::ProjectEdit || c == McpToolClass::Analysis ||
                c == McpToolClass::Unjournaled || c == McpToolClass::Session ||
                (c == McpToolClass::FileWrite &&
                 (tool.name == "export_timeline" ||
                  tool.name == "export_timelines")) ||
                // GENERATIF-2026-10 -- a paid generation that edits the
                // library: a lost answer replays, it never spends twice.
                (c == McpToolClass::External && tool.name == "generate_media"),
            name +
                ": a tool that takes an idempotency_key changes "
                "something");
        const bool journaled = c == McpToolClass::JournaledEdit ||
                               c == McpToolClass::ProjectEdit ||
                               c == McpToolClass::Analysis;
        Check(!journaled || tool.idempotent ||
                  editsWithoutKey.count(tool.name) != 0,
              name + ": an edit or an analysis takes an idempotency_key");
    }

    // A2: the annotations are the protocol's, only where they differ from
    // its defaults (readOnlyHint false, destructiveHint true,
    // idempotentHint false, openWorldHint true), and follow from the class.
    std::map<std::string, size_t> perClass;
    for (const McpTool& tool : registry.Tools()) {
        ++perClass[McpToolClassName(tool.tool_class)];
        Value listing;
        std::string parseError;
        Check(Value::Parse(McpToolListingJson(tool), listing, parseError),
              tool.name + " is listed as JSON: " + parseError);
        const Value* annotations = listing.Find("annotations");
        Check(annotations != nullptr && annotations->IsObject(),
              tool.name + " carries an annotations object");
        if (annotations == nullptr || !annotations->IsObject()) continue;
        std::map<std::string, bool> hints;
        for (const auto& [key, value] : annotations->AsObject()) {
            Check(value.IsBool(), tool.name + "." + key + " is a boolean");
            hints[key] = value.IsBool() && value.AsBool();
        }
        const auto has = [&](const char* key) { return hints.count(key) != 0; };
        Check((!has("readOnlyHint") || hints["readOnlyHint"]) &&
                  (!has("destructiveHint") || !hints["destructiveHint"]) &&
                  !has("idempotentHint") &&
                  (!has("openWorldHint") || !hints["openWorldHint"]) &&
                  hints.size() <= 2,
              tool.name + " says only what differs from the defaults: " +
                  annotations->Dump());
        const McpToolClass c = tool.tool_class;
        const bool read =
            c == McpToolClass::Read || c == McpToolClass::ExternalRead;
        const bool external =
            c == McpToolClass::External || c == McpToolClass::ExternalRead;
        const bool undone = c == McpToolClass::JournaledEdit ||
                            c == McpToolClass::Analysis ||
                            c == McpToolClass::Session;
        Check(has("readOnlyHint") == read && has("openWorldHint") != external &&
                  has("destructiveHint") == undone,
              tool.name + "'s annotations follow from its class " +
                  McpToolClassName(c) + ": " + annotations->Dump());
    }
    Check(perClass["unclassified"] == 0, "no tool is left unclassified");

    // The two listings are one: the server's tools/list and `--tools`.
    InMemoryBackend backend(fixture);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError), "a server starts: " + startError);
    Value served;
    std::string parseError;
    Check(
        Value::Parse(HttpBody(HttpPostJson(
                         server.Port(), "/mcp",
                         R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")),
                     served, parseError),
        "tools/list answers JSON: " + parseError);
    server.Stop();
    std::string printed;
    Value listed;
    Check(ListToolsCommand(printed) == 0 &&
              Value::Parse(printed, listed, parseError),
          "--tools prints JSON: " + parseError);
    const Value* servedTools =
        served.Find("result") ? served.Find("result")->Find("tools") : nullptr;
    const Value* printedTools = listed.Find("tools");
    Check(servedTools != nullptr && printedTools != nullptr &&
              servedTools->Dump() == printedTools->Dump(),
          "tools/list and --tools publish the same tools, annotations "
          "included");

    // A4: the agent panel lists the tools for its model itself, and sends
    // no annotations to any provider.
    for (const chat::ChatLlmProvider provider :
         {chat::ChatLlmProvider::AnthropicMessages,
          chat::ChatLlmProvider::OpenAiCompatible,
          chat::ChatLlmProvider::Ollama}) {
        chat::ChatLlmConfig config;
        config.provider = provider;
        config.api_key = "test-key";
        config.base_url = "http://127.0.0.1:9";
        std::string body;
        chat::ChatLlmClient client(
            config, [&](const chat::ChatHttpRequest& request,
                        chat::ChatHttpResponse& response, std::string&) {
                body = request.body;
                response.status_code = 500;
                response.body = "{}";
                return true;
            });
        chat::ChatMessage message;
        message.role = "user";
        message.content.push_back(
            chat::ChatContentBlock::MakeText("Pose le premier plan."));
        client.SendMessages({message}, "system", registry.Tools());
        Value request;
        Check(Value::Parse(body, request, parseError) &&
                  body.find("\"insert_clip\"") != std::string::npos &&
                  body.find("annotations") == std::string::npos &&
                  body.find("readOnlyHint") == std::string::npos &&
                  body.find("openWorldHint") == std::string::npos,
              "the panel's request carries the tools and no annotation");
    }
}

// B5: a call with nothing names, as required, only what the schema
// requires; the arguments are then given one by one, from the schema,
// until the tool stops asking.
// The fixture's rush as a library media too: what framing, M1 and the
// sample arguments need.
Document FixtureWithLibrary(const Document& fixture) {
    Document document = fixture;
    LibraryMedia media;
    media.id = document.sources[0].id;
    media.path = document.sources[0].path;
    media.filename = "A.MP4";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.rate = document.sources[0].rate;
    media.duration = document.sources[0].duration;
    media.orientation = "landscape";
    document.library = {media};
    return document;
}

void CheckRequiredArguments(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const Document document = FixtureWithLibrary(fixture);
    for (const McpTool& tool : registry.Tools()) {
        Value schema;
        std::string parseError;
        if (!Value::Parse(tool.input_schema_json, schema, parseError)) continue;
        const Value* properties = schema.Find("properties");
        std::set<std::string> required;
        if (const Value* list = schema.Find("required")) {
            for (const Value& name : list->AsArray()) {
                required.insert(name.AsString());
                Check(properties != nullptr &&
                          properties->Find(name.AsString()) != nullptr,
                      tool.name + " requires '" + name.AsString() +
                          "', which it declares");
            }
        }
        Value arguments = Value::MakeObject();
        const std::string prefix = "'" + tool.name + ".";
        for (int round = 0; round < 12; ++round) {
            InMemoryBackend backend(document);
            const McpToolCallOutcome outcome =
                registry.Call(backend, tool.name, arguments);
            const size_t at = outcome.message.find(prefix);
            const size_t end = outcome.message.find("' is required");
            if (outcome.ok || at != 0 || end == std::string::npos) break;
            const std::string name =
                outcome.message.substr(prefix.size(), end - prefix.size());
            if (name.find_first_of(".[") != std::string::npos) break;
            Check(required.count(name) != 0,
                  tool.name + " asks for '" + name +
                      "' without listing it in its schema's required");
            const Value* property =
                properties != nullptr ? properties->Find(name) : nullptr;
            if (property == nullptr || arguments.Find(name) != nullptr) break;
            arguments.Set(name, SampleValue(*property, name));
        }
    }
}

// The 2,048 characters Claude Code reads of a description; the catalog
// keeps under 2,000.
void CheckDescriptionLengths() {
    const McpToolRegistry registry;
    for (const McpTool& tool : registry.Tools()) {
        Check(CharacterCount(tool.description) <= 2048,
              tool.name + "'s description is cut by Claude Code: " +
                  std::to_string(CharacterCount(tool.description)) +
                  " characters");
        mcp_json::Value schema;
        std::string parseError;
        if (!mcp_json::Value::Parse(tool.input_schema_json, schema, parseError))
            continue;
        std::vector<SchemaSite> sites;
        std::vector<std::string> path;
        CollectSchemaSites(schema, path, false, sites);
        for (const SchemaSite& site : sites) {
            const mcp_json::Value* description =
                site.schema->Find("description");
            if (description == nullptr || !description->IsString()) continue;
            Check(CharacterCount(description->AsString()) <= 2048,
                  SitePath(tool.name, site) +
                      "'s description is cut by Claude Code");
        }
    }
}

// PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. What a profile
// serves of the catalog, what it refuses, and what its server says.
void CheckToolProfiles(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry full;
    Check(full.Profile().everything && full.Profile().name == "full" &&
              full.Tools().size() == full.CatalogSize(),
          "a registry serves the whole catalog by default");
    Check(FindMcpToolProfile("full") == &FullMcpToolProfile() &&
              FindMcpToolProfile("motion") != nullptr &&
              FindMcpToolProfile("itw") != nullptr &&
              FindMcpToolProfile("reel") != nullptr &&
              FindMcpToolProfile("nope") == nullptr &&
              McpToolProfileNames() == "full, motion, itw, reel",
          "the profiles are found by name, and only theirs: " +
              McpToolProfileNames());

    // Every tool records the domain of the file that registers it.
    const std::vector<std::string> domains = McpToolDomains();
    std::set<std::string> seenDomains;
    std::string undomained;
    for (const McpTool& tool : full.Tools()) {
        if (std::find(domains.begin(), domains.end(), tool.domain) ==
            domains.end())
            undomained += " " + tool.name + "(" + tool.domain + ")";
        seenDomains.insert(tool.domain);
    }
    Check(undomained.empty(),
          "every tool has one of the domains:" + undomained);
    Check(seenDomains.size() == domains.size(),
          "every domain has its tools: " + std::to_string(seenDomains.size()) +
              " of " + std::to_string(domains.size()));
    const auto domainOf = [&](const std::string& name) {
        for (const McpTool& tool : full.Tools()) {
            if (tool.name == name) return tool.domain;
        }
        return std::string("?");
    };
    Check(domainOf("after_set_text") == mcp_domain::kAfter &&
              domainOf("render_motion_frame") == mcp_domain::kMotion &&
              domainOf("place_motion_clip") == mcp_domain::kMotionClip &&
              domainOf("undo") == mcp_domain::kSession &&
              domainOf("list_motifs") == mcp_domain::kSession &&
              domainOf("remove_words") == mcp_domain::kSpeech &&
              domainOf("generate_media") == mcp_domain::kGeneration &&
              domainOf("generate_voice_over") == mcp_domain::kVoice &&
              domainOf("describe") == mcp_domain::kProject,
          "a tool's domain is the one of the file that registers it");

    // What each profile names is in the catalog, and its line is ASCII.
    for (const McpToolProfile& profile : McpToolProfiles()) {
        for (const std::string& domain : profile.domains) {
            Check(std::find(domains.begin(), domains.end(), domain) !=
                      domains.end(),
                  profile.name + " names a domain of the catalog: " + domain);
        }
        for (const std::string& tool : profile.tools) {
            Check(full.Offers(tool),
                  profile.name + " names a tool of the catalog: " + tool);
        }
        Check(std::all_of(
                  profile.summary.begin(), profile.summary.end(),
                  [](char c) { return static_cast<unsigned char>(c) < 0x80; }),
              profile.name + "'s summary is ASCII, as the instructions are");
    }

    const McpToolProfile& motionProfile = *FindMcpToolProfile("motion");
    const McpToolRegistry motion(motionProfile);
    Check(&motion.Profile() == &motionProfile &&
              motion.CatalogSize() == full.Tools().size() &&
              motion.Tools().size() < full.Tools().size() / 2,
          "the motion profile serves less than half the catalog: " +
              std::to_string(motion.Tools().size()) + " of " +
              std::to_string(motion.CatalogSize()));
    std::string missing;
    for (const McpTool& tool : full.Tools()) {
        const bool whole = tool.domain == mcp_domain::kMotion ||
                           tool.domain == mcp_domain::kMotionClip ||
                           tool.domain == mcp_domain::kAfter;
        if (whole && !motion.Offers(tool.name)) missing += " " + tool.name;
    }
    Check(missing.empty(),
          "it serves motion, motion clips and After Effects whole:" + missing);
    for (const char* kept :
         {"create_project", "open_project", "describe", "get_procedure", "undo",
          "export_timeline", "render_motion_video", "after_render_frame",
          "list_tempo_grid", "check_timeline"}) {
        Check(motion.Offers(kept), std::string("motion serves ") + kept);
    }
    for (const char* left :
         {"remove_words", "transcribe_media", "generate_media", "propose_grade",
          "get_editor_view"}) {
        Check(!motion.Offers(left), std::string("motion leaves out ") + left);
    }
    // A tool it keeps is the full catalog's, marking included.
    for (const McpTool& tool : motion.Tools()) {
        const auto same = std::find_if(
            full.Tools().begin(), full.Tools().end(),
            [&](const McpTool& entry) { return entry.name == tool.name; });
        Check(same != full.Tools().end() &&
                  same->description == tool.description &&
                  same->input_schema_json == tool.input_schema_json &&
                  same->tool_class == tool.tool_class &&
                  same->background == tool.background &&
                  same->idempotent == tool.idempotent,
              tool.name + " is served as the full catalog describes it");
    }

    // The motion-design procedure never sends an agent after a tool its
    // profile left out -- but for the one it names to send the work
    // elsewhere: subtitles are not a composition (references/motifs.md,
    // section 17), they are monter-une-itw's, with its transcription.
    size_t fileCount = 0;
    const EmbeddedProcedureFile* files = EmbeddedProcedureFiles(fileCount);
    const auto checkProcedure =
        [&](const std::string& procedure, const McpToolRegistry& registry,
            const std::set<std::string>& sentElsewhere) {
            size_t procedureFiles = 0;
            std::set<std::string> unserved;
            for (size_t index = 0; index < fileCount; ++index) {
                if (std::string(files[index].procedure) != procedure) continue;
                ++procedureFiles;
                const std::string text(
                    reinterpret_cast<const char*>(files[index].bytes),
                    files[index].size);
                for (size_t at = 0; at < text.size();) {
                    const auto isWordChar = [](char c) {
                        return std::islower(static_cast<unsigned char>(c)) ||
                               std::isdigit(static_cast<unsigned char>(c)) ||
                               c == '_';
                    };
                    if (!isWordChar(text[at])) {
                        ++at;
                        continue;
                    }
                    size_t end = at;
                    while (end < text.size() && isWordChar(text[end])) ++end;
                    const std::string word = text.substr(at, end - at);
                    if (full.Offers(word) && !registry.Offers(word) &&
                        sentElsewhere.count(word) == 0)
                        unserved.insert(word);
                    at = end;
                }
            }
            std::string unservedList;
            for (const std::string& word : unserved) unservedList += " " + word;
            Check(
                procedureFiles > 0 && unserved.empty(),
                "every tool the " + procedure + " procedure names is served (" +
                    std::to_string(procedureFiles) + " files):" + unservedList);
        };
    checkProcedure("motion-design", motion, {"generate_captions"});

    // PA10 -- docs/roadmap/PARITE-2026-11.md. The two editing profiles:
    // each serves the domains it names whole, every tool its procedure
    // names, and leaves the other trades out.
    const McpToolRegistry itw(*FindMcpToolProfile("itw"));
    const McpToolRegistry reel(*FindMcpToolProfile("reel"));
    checkProcedure("monter-une-itw", itw, {});
    checkProcedure("monter-un-reel", reel, {});
    const auto servesWhole = [&](const McpToolRegistry& registry,
                                 const std::set<std::string>& whole) {
        std::string left;
        for (const McpTool& tool : full.Tools())
            if (whole.count(tool.domain) != 0 && !registry.Offers(tool.name))
                left += " " + tool.name;
        return left;
    };
    Check(servesWhole(itw, {mcp_domain::kSpeech, mcp_domain::kCaption,
                            mcp_domain::kSession})
              .empty(),
          "itw serves speech, captions and the session whole");
    Check(servesWhole(reel, {mcp_domain::kClip, mcp_domain::kPicture,
                             mcp_domain::kSound, mcp_domain::kGeneration,
                             mcp_domain::kMotionClip, mcp_domain::kSession})
              .empty(),
          "reel serves clip settings, pictures, sound, generation, motion "
          "clips and the session whole");
    Check(itw.Tools().size() * 5 < full.Tools().size() * 3 &&
              reel.Tools().size() * 5 < full.Tools().size() * 3,
          "each serves less than three fifths of the catalog: itw " +
              std::to_string(itw.Tools().size()) + ", reel " +
              std::to_string(reel.Tools().size()) + " of " +
              std::to_string(full.Tools().size()));
    for (const char* left : {"after_set_text", "edit_motion_composition",
                             "generate_media", "set_clip_speed"})
        Check(!itw.Offers(left), std::string("itw leaves out ") + left);
    for (const char* left : {"after_set_text", "edit_motion_composition",
                             "remove_words", "transcribe_media", "apply_cdl"})
        Check(!reel.Offers(left), std::string("reel leaves out ") + left);
    for (const char* kept :
         {"remove_words", "generate_captions", "undo", "export_timeline",
          "review_cuts", "propose_grade", "get_editor_view"})
        Check(itw.Offers(kept), std::string("itw serves ") + kept);
    for (const char* kept :
         {"set_clip_speed", "analyze_audio_beats", "generate_media", "undo",
          "export_timelines", "review_cuts"})
        Check(reel.Offers(kept), std::string("reel serves ") + kept);

    // A tool the profile left out is refused by name, a tool nobody has as
    // unknown, and a tool it serves runs.
    InMemoryBackend backend(fixture);
    const McpToolCallOutcome refused =
        motion.Call(backend, "remove_words", Value::MakeObject());
    Check(!refused.ok && refused.error_name == "ToolNotInProfile" &&
              refused.message.find("'motion'") != std::string::npos &&
              refused.message.find("without --profile") != std::string::npos,
          "a tool outside the profile is refused as such: " + refused.message);
    const McpToolCallOutcome unknown =
        motion.Call(backend, "no_such_tool", Value::MakeObject());
    Check(!unknown.ok && unknown.error_name == "UnknownTool",
          "a tool the catalog does not have stays unknown");
    const McpToolCallOutcome described =
        motion.Call(backend, "list_timelines", Value::MakeObject());
    Check(described.ok, "a tool it serves runs: " + described.message);

    // Its server lists what it serves, refuses the rest, and names only what
    // it serves in its instructions.
    const auto parse = [](const std::string& text) {
        Value parsed;
        std::string error;
        Value::Parse(text, parsed, error);
        return parsed;
    };
    McpServer server(backend, motionProfile);
    Check(server.Tools().Tools().size() == motion.Tools().size(),
          "the server serves the profile");
    std::string answer;
    server.HandleJsonRpc(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                         answer);
    const Value listed = parse(answer);
    const Value* listedTools =
        listed.Find("result") ? listed.Find("result")->Find("tools") : nullptr;
    Check(listedTools != nullptr &&
              listedTools->AsArray().size() == motion.Tools().size(),
          "tools/list lists the profile's tools");
    server.HandleJsonRpc(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":)"
        R"({"name":"remove_words","arguments":{}}})",
        answer);
    Check(answer.find("ToolNotInProfile") != std::string::npos &&
              answer.find("\"isError\":true") != std::string::npos,
          "tools/call refuses a tool outside the profile: " + answer);
    const auto instructionsOf = [&](McpServer& of) {
        std::string initialized;
        of.HandleJsonRpc(
            R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":)"
            R"({"protocolVersion":"2025-06-18"}})",
            initialized);
        const Value parsed = parse(initialized);
        const Value* result = parsed.Find("result");
        return result != nullptr && result->Find("instructions") != nullptr
                   ? result->Find("instructions")->AsString()
                   : std::string();
    };
    const std::string instructions = instructionsOf(server);
    Check(instructions.find("This server runs the `motion` profile: " +
                            std::to_string(motion.Tools().size()) + " of " +
                            std::to_string(motion.CatalogSize()) + " tools") !=
                  std::string::npos &&
              instructions.find("ToolNotInProfile") != std::string::npos &&
              instructions.find("`get_procedure`") != std::string::npos,
          "the instructions say the profile: " + instructions);
    Check(instructions.size() <= 2048,
          "they fit in the 2,048 characters a client keeps: " +
              std::to_string(instructions.size()));
    const size_t map = instructions.find("Tools by task:");
    std::string unservedNamed;
    size_t named = 0;
    if (map != std::string::npos) {
        const std::string section = instructions.substr(map);
        for (size_t at = 0; at < section.size();) {
            const size_t begin =
                section.find_first_of("abcdefghijklmnopqrstuvwxyz", at);
            if (begin == std::string::npos) break;
            size_t end = begin;
            while (end < section.size() &&
                   (std::islower(static_cast<unsigned char>(section[end])) ||
                    section[end] == '_'))
                ++end;
            const std::string word = section.substr(begin, end - begin);
            if (word.find('_') != std::string::npos) {
                ++named;
                if (!motion.Offers(word)) unservedNamed += " " + word;
            }
            at = end + 1;
        }
    }
    Check(map != std::string::npos && named >= 10 && unservedNamed.empty(),
          "its map names only what it serves (" + std::to_string(named) +
              " named):" + unservedNamed);
    Check(instructions.find("- data:") == std::string::npos &&
              instructions.find("- in the app:") == std::string::npos,
          "and drops a line it serves none of");
    McpServer fullServer(backend);
    Check(instructionsOf(fullServer).find("profile") == std::string::npos,
          "a server of the whole catalog says nothing of a profile");
}

// The catalog's weight: what PRD15 took out stays out.
void CheckCatalogWeightPosts() {
    const McpToolRegistry registry;
    for (const McpTool& tool : registry.Tools()) {
        const std::string& schema = tool.input_schema_json;
        Check(schema.find("\"description\":\"" + tool.name + " arguments\"") ==
                      std::string::npos &&
                  schema.find("\"description\":\"" + tool.name +
                              " takes no arguments\"") == std::string::npos,
              tool.name + " has no boilerplate schema description");
        Check(schema.find("\"required\":[]") == std::string::npos,
              tool.name + " publishes no empty required");
        Check(schema.find("Accepts a full ID or any unambiguous prefix") ==
                  std::string::npos,
              tool.name + " uses the short ID phrase");
    }
}

// PA5, PA9 -- docs/roadmap/PARITE-2026-11.md. A caption track laid in
// another language, and what a timeline became since a state.
void CheckTranslationAndChanges(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto call = [&](McpBackend& on, const std::string& tool,
                          const std::string& json) {
        return registry.Call(on, tool, ParsedJson(json));
    };
    const auto parsed = [](const McpToolCallOutcome& outcome) {
        Value value;
        std::string parseError;
        Value::Parse(outcome.result_json, value, parseError);
        return value;
    };
    const auto text = [](const Value& value, const std::string& key) {
        const Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    const auto flag = [](const Value& value, const std::string& key) {
        const Value* field = value.Find(key);
        return field != nullptr && field->IsBool() && field->AsBool();
    };

    const Ulid captions = "01K300000000000000000000C0";
    const Ulid first = "01K300000000000000000000C1";
    const Ulid second = "01K300000000000000000000C2";
    Document document = fixture;
    CaptionStyle style;
    style.id = "01K300000000000000000000C9";
    document.sequence.caption_styles = {style};
    DocumentTrack track;
    track.id = captions;
    track.kind = "caption";
    track.index = 2;
    track.sync_lock = true;
    for (const auto& [id, in, words] :
         std::vector<std::tuple<Ulid, int64_t, std::string>>{
             {first, 5, "Bonjour à tous"}, {second, 12, "et merci"}}) {
        DocumentClip carton;
        carton.id = id;
        carton.timeline_in = {in, 25};
        carton.duration = {5, 25};
        carton.source_in = {0, 25};
        carton.include_audio = false;
        carton.caption_group_id = style.id;
        carton.caption_text = words;
        // PA4 -- the source's words are timed, a frame apart.
        for (size_t word = 0; word < CaptionWordCount(words); ++word)
            carton.caption_word_times.push_back(
                ReducedCaptionWordTime({static_cast<int64_t>(word), 25}));
        track.clips.push_back(carton);
    }
    document.sequence.tracks.push_back(track);

    {
        InMemoryBackend backend(document);
        const std::string before = backend.CurrentDocument().SaveToString();
        const std::string both =
            R"({"track_id":")" + captions +
            R"(","language":"en","cartons":[{"clip_id":")" + first +
            R"(","text":"Hello everyone"},{"clip_id":")" + second +
            R"(","text":"and thank you"}])";
        const McpToolCallOutcome preview =
            call(backend, "translate_captions", both + R"(,"preview":true})");
        Check(
            preview.ok && !flag(parsed(preview), "applied") &&
                backend.CurrentDocument().SaveToString() == before,
            "translate_captions previews without editing: " + preview.message);

        const McpToolCallOutcome missing =
            call(backend, "translate_captions",
                 R"({"track_id":")" + captions +
                     R"(","language":"en","cartons":[{"clip_id":")" + first +
                     R"(","text":"Hello everyone"}]})");
        Check(!missing.ok &&
                  missing.message.find("1 of 2 cartons have no translation") !=
                      std::string::npos &&
                  missing.message.find(second) != std::string::npos,
              "a carton left without a text is refused by name: " +
                  missing.message);
        CheckFailureEnvelope(call(backend, "translate_captions",
                                  R"({"track_id":"01K30000000000000000000002",)"
                                  R"("language":"en","cartons":[{"clip_id":")" +
                                      first + R"(","text":"Hello"}]})"),
                             "translate_captions on a picture track",
                             "ValidationFailed");
        CheckFailureEnvelope(
            call(backend, "translate_captions",
                 R"({"track_id":")" + captions +
                     R"(","language":"anglais !","cartons":[{"clip_id":")" +
                     first + R"(","text":"Hello"}]})"),
            "translate_captions in a language that is no tag",
            "ValidationFailed");

        const McpToolCallOutcome laid =
            call(backend, "translate_captions", both + "}");
        Check(laid.ok, "translate_captions lays the track: " + laid.message);
        const Value answer = parsed(laid);
        const auto& tracks = backend.CurrentDocument().sequence.tracks;
        Check(tracks.size() == 4 && tracks.back().kind == "caption" &&
                  !tracks.back().visible && tracks.back().sync_lock &&
                  tracks.back().id == text(answer, "track_id") &&
                  tracks.back().clips.size() == 2,
              "one hidden caption track is added, in the source's sync lock");
        if (tracks.size() == 4 && tracks.back().clips.size() == 2) {
            const DocumentClip& one = tracks.back().clips[0];
            const DocumentClip& two = tracks.back().clips[1];
            // PA4 -- "Bonjour à tous" is three words and "Hello everyone"
            // two: the source's word times are not the translation's.
            Check(one.caption_word_times.empty() &&
                      two.caption_word_times.empty() &&
                      tracks[tracks.size() - 2]
                              .clips[0]
                              .caption_word_times.size() == 3,
                  "a translated carton carries no word time; the source "
                  "keeps its own");
            Check(one.caption_text == "Hello everyone" &&
                      two.caption_text == "and thank you" &&
                      one.timeline_in == RationalTime(5, 25) &&
                      two.timeline_in == RationalTime(12, 25) &&
                      one.duration == RationalTime(5, 25) &&
                      one.caption_group_id == style.id && one.id != first &&
                      two.id != second,
                  "its cartons keep the source's times and style, with "
                  "their own identities");
        }
        const Value* cartons = answer.Find("cartons");
        Check(cartons != nullptr && cartons->IsArray() &&
                  cartons->AsArray().size() == 2 &&
                  text(cartons->AsArray()[0], "source_clip_id") == first &&
                  cartons->AsArray()[0].Find("characters")->Dump() == "14" &&
                  // 14 characters in 5 frames at 25 i/s: 70 per second.
                  cartons->AsArray()[0].Find("characters_per_second")->Dump() ==
                      "70.0",
              "each carton says its length and its rate: " + laid.result_json);
        // A test binary installs no caption renderer: the widths are said
        // unmeasured, and the edit stands.
        Check(!flag(answer, "widths_measured") &&
                  !text(answer, "width_error").empty() &&
                  flag(answer, "applied"),
              "without the caption renderer the widths are said unmeasured");
        const auto& entries = backend.Log().AppliedEntries();
        Check(!entries.empty() && entries.back().motifs.size() == 1 &&
                  entries.back().motifs[0].motif.reason ==
                      MotifReason::Captions &&
                  entries.back().motifs[0].motif.rule == "translate_captions" &&
                  entries.back().motifs[0].motif.params.Dump().find(
                      "\"language\":\"en\"") != std::string::npos,
              "the journal keeps the language with the edit");
        Check(call(backend, "undo", "{}").ok &&
                  backend.CurrentDocument().SaveToString() == before,
              "one undo removes the translated track, to the byte");
    }

    {
        InMemoryBackend backend(fixture);
        const std::string start =
            text(parsed(call(backend, "list_changes", "{}")), "doc_hash");
        Check(start.size() == 16,
              "list_changes without `since` names the current state: " + start);
        const McpToolCallOutcome marked =
            call(backend, "add_marker",
                 R"({"name":"Ici","time":{"value":3,"rate":25}})");
        Check(marked.ok, "a marker is added: " + marked.message);
        // This backend's edits answer no doc_hash; the real ones do, and
        // it is the one list_changes names.
        const std::string after =
            text(parsed(call(backend, "list_changes", "{}")), "doc_hash");
        Check(after.size() == 16 && after != start,
              "an edit leaves another state: " + after);

        const Value since = parsed(
            call(backend, "list_changes", R"({"since":")" + start + R"("})"));
        const Value* entries = since.Find("entries");
        const Value* delta = since.Find("delta");
        Check(flag(since, "found") && flag(since, "changed") &&
                  !flag(since, "undone") && text(since, "doc_hash") == after &&
                  entries != nullptr && entries->AsArray().size() == 1 &&
                  text(entries->AsArray()[0], "type") == "AddMarker" &&
                  delta != nullptr &&
                  delta->Dump().find("marker") != std::string::npos,
              "what one edit changed is one delta and one entry: " +
                  since.Dump());
        const Value same = parsed(
            call(backend, "list_changes", R"({"since":")" + after + R"("})"));
        Check(flag(same, "found") && !flag(same, "changed") &&
                  same.Find("entries")->AsArray().empty() &&
                  same.Find("delta") == nullptr,
              "nothing changed since the current state");

        Check(call(backend, "undo", "{}").ok, "the marker is undone");
        const Value back = parsed(
            call(backend, "list_changes", R"({"since":")" + after + R"("})"));
        Check(flag(back, "found") && flag(back, "undone") &&
                  flag(back, "changed") &&
                  back.Find("entries")->AsArray().size() == 1 &&
                  text(back, "doc_hash") == start,
              "a state the person undid past is found ahead of the cursor: " +
                  back.Dump());

        Check(call(backend, "add_marker",
                   R"({"name":"Ailleurs","time":{"value":7,"rate":25}})")
                  .ok,
              "another edit rewrites the history");
        const Value lost = parsed(
            call(backend, "list_changes", R"({"since":")" + after + R"("})"));
        Check(!flag(lost, "found") &&
                  text(lost, "note").find("describe") != std::string::npos &&
                  lost.Find("delta") == nullptr,
              "a state the journal no longer leads to is not found, and "
              "says to read again: " +
                  lost.Dump());
        CheckFailureEnvelope(
            call(backend, "list_changes", R"({"since":"hier"})"),
            "list_changes since something that is no doc_hash",
            "ValidationFailed");
    }
}

// Out of lot -- the author, 4 November 2026 (LinkGroupRepair.h). The fixture's
// two shots share one link without overlapping, the shape a cut left before
// #213; each gets its sound here, under the same link. split_link_groups
// counts on preview, then splits in one undo step, and finds nothing after.
void CheckSplitLinkGroups(const Document& fixture) {
    const Ulid block = "01K30000000000000000000005";
    const Ulid secondSound = "01K30000000000000000000077";
    Document legacy = fixture;
    DocumentTrack& sounds = legacy.sequence.tracks[1];
    sounds.clips[0].link_group_id = block;
    DocumentClip second = sounds.clips[0];
    second.id = secondSound;
    second.source_in = {200, 25};
    second.timeline_in = {20, 25};
    sounds.clips.push_back(second);
    const McpToolRegistry registry;
    InMemoryBackend backend(legacy);
    const std::string before = backend.CurrentDocument().SaveToString();
    const auto count = [](const McpToolCallOutcome& outcome,
                          const std::string& key) {
        mcp_json::Value answer;
        std::string parseError;
        int64_t value = -1;
        if (mcp_json::Value::Parse(outcome.result_json, answer, parseError)) {
            if (const mcp_json::Value* field = answer.Find(key))
                field->AsInt64(value);
        }
        return value;
    };

    const McpToolCallOutcome preview = registry.Call(
        backend, "split_link_groups", ParsedJson(R"({"preview":true})"));
    Check(preview.ok && count(preview, "groups") == 1 &&
              count(preview, "places") == 1 &&
              count(preview, "operations") == 2 &&
              preview.result_json.find("\"applied\":false") !=
                  std::string::npos &&
              backend.CurrentDocument().SaveToString() == before,
          "split_link_groups previews one group, one place, two links, and "
          "changes nothing: " +
              preview.result_json + preview.message);

    const McpToolCallOutcome split = registry.Call(
        backend, "split_link_groups", mcp_json::Value::MakeObject());
    const Document& after = backend.CurrentDocument();
    const DocumentClip* firstShot =
        after.FindClip("01K30000000000000000000003");
    const DocumentClip* firstSound =
        after.FindClip("01K30000000000000000000006");
    const DocumentClip* secondShot =
        after.FindClip("01K30000000000000000000004");
    const DocumentClip* secondHeard = after.FindClip(secondSound);
    Check(split.ok && firstShot && firstSound && secondShot && secondHeard &&
              firstShot->link_group_id == block &&
              firstSound->link_group_id == block &&
              secondShot->link_group_id == secondHeard->link_group_id &&
              !secondShot->link_group_id.empty() &&
              secondShot->link_group_id != block,
          "each shot and its sound share a group of their own: " +
              split.result_json + split.message);
    Check(count(registry.Call(backend, "split_link_groups",
                              mcp_json::Value::MakeObject()),
                "groups") == 0,
          "split once, the timeline has nothing left to split");
    Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok &&
              backend.CurrentDocument().SaveToString() == before,
          "one undo gives the shared link back, byte for byte");
    Check(!registry
               .Call(backend, "split_link_groups",
                     ParsedJson(R"({"previews":true})"))
               .ok,
          "a misspelled argument is refused rather than ignored");
}

// Out of lot -- the author, 29 October 2026: set_clip_enabled, Resolve's
// Enable/Disable Clip and the window's D, through the tool an agent calls.
void CheckSetClipEnabled(const Document& fixture) {
    const McpToolRegistry registry;
    InMemoryBackend backend(fixture);
    const std::string original = backend.CurrentDocument().SaveToString();
    // describe is cli_tests' to check: this backend describes the project
    // it was built from.
    const auto disabledKeys = [](const std::string& text) {
        size_t count = 0;
        for (size_t at = text.find("\"enabled\":false");
             at != std::string::npos;
             at = text.find("\"enabled\":false", at + 1))
            ++count;
        return count;
    };
    const McpToolCallOutcome disabled = registry.Call(
        backend, "set_clip_enabled",
        ParsedJson(R"({"clip_ids":["01K30000000000000000000003",)"
                   R"("01K30000000000000000000006"],"enabled":false})"));
    Check(disabled.ok, "set_clip_enabled disables a picture and its sound: " +
                           disabled.message);
    const Document& after = backend.CurrentDocument();
    Check(!after.sequence.tracks[0].clips[0].enabled &&
              after.sequence.tracks[0].clips[1].enabled &&
              !after.sequence.tracks[1].clips[0].enabled,
          "the two clips named are disabled, the other is not");
    Check(disabledKeys(disabled.result_json) == 2,
          "the delta says so of both: " + disabled.result_json);
    Check(
        !registry
             .Call(backend, "set_clip_enabled",
                   ParsedJson(R"({"clip_ids":["01K30000000000000000000003"]})"))
             .ok,
        "a call that does not say which state is refused");
    Check(!registry
               .Call(backend, "set_clip_enabled",
                     ParsedJson(R"({"clip_ids":[],"enabled":true})"))
               .ok,
          "an empty clip list is refused");
    Check(!registry
               .Call(backend, "set_clip_enabled",
                     ParsedJson(R"({"clip_ids":["01K30000000000000000000003"],)"
                                R"("enabled":false,"linked":true})"))
               .ok,
          "a misspelled argument is refused rather than ignored");
    Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok &&
              backend.CurrentDocument().SaveToString() == original,
          "one undo enables both again, byte for byte");
}

// PRD15 -- the closed sets the code accepted and the schema did not
// declare, now enumerations that take any case; the sync warning in
// English; and the alias of a track an edit creates.
void CheckDeclaredSetsAndSmallFixes(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto normalized = [&](const std::string& tool,
                                const std::string& json) {
        Value result;
        std::string message;
        registry.NormalizeArguments(tool, ParsedJson(json), result, message);
        return result.Dump();
    };
    Check(normalized("set_clip_framing", R"({"mode":"FILL"})") ==
                  R"({"mode":"fill"})" &&
              normalized("add_transition", R"({"type":"Dip_To_Color"})") ==
                  R"({"type":"dip_to_color"})" &&
              normalized("update_transition", R"({"type":"CROSS_DISSOLVE"})") ==
                  R"({"type":"cross_dissolve"})" &&
              normalized("set_sequence_lut", R"({"space":"ACEScct"})") ==
                  R"({"space":"acescct"})" &&
              normalized(
                  "set_clip_animation",
                  R"({"animation":[{"parameter":"Framing.Scale",)"
                  R"("keyframes":[{"interpolation":"Cubic-Bezier"}]}]})") ==
                  R"({"animation":[{"parameter":"framing.scale",)"
                  R"("keyframes":[{"interpolation":"cubic-bezier"}]}]})",
          "the closed sets the code accepts are enumerations that take "
          "any case");
    {
        InMemoryBackend backend(FixtureWithLibrary(fixture));
        const McpToolCallOutcome framed =
            registry.Call(backend, "set_clip_framing",
                          ParsedJson(R"({"clip_ids":["A1"],"mode":"Fill"})"));
        Check(
            framed.ok && backend.CurrentDocument()
                                 .sequence.tracks[0]
                                 .clips[0]
                                 .framing.mode == FramingMode::Fill,
            "set_clip_framing takes a mode in another case: " + framed.message);
    }
    {
        // insert_clip at the start: the audio track holds material after
        // it and is not asked to follow.
        InMemoryBackend backend(fixture);
        const McpToolCallOutcome inserted = registry.Call(
            backend, "insert_clip",
            ParsedJson(R"({"track_id":"T1","source_id":)"
                       R"("01K30000000000000000000001","source_in":)"
                       R"({"value":0,"rate":25},"duration":{"value":5,)"
                       R"("rate":25},"timeline_in":{"value":0,"rate":25}})"));
        Value result;
        std::string parseError;
        Check(inserted.ok &&
                  Value::Parse(inserted.result_json, result, parseError) &&
                  result.Find("warning") != nullptr &&
                  result.Find("warning")->AsString().rfind(
                      "sync_track_ids omitted:", 0) == 0,
              "the sync warning speaks English: " + inserted.result_json);
    }
    {
        // A plan's delta is computed by the engine: the track it adds
        // comes back with its alias.
        InMemoryBackend backend(fixture);
        const McpToolCallOutcome prepared = registry.Call(
            backend, "prepare_edit_plan",
            ParsedJson(R"({"label":"Piste","steps":[{"tool":"add_track",)"
                       R"("arguments":{"kind":"video"}}]})"));
        Value preview;
        std::string parseError;
        const Value* created = nullptr;
        if (Value::Parse(prepared.result_json, preview, parseError) &&
            preview.Find("delta") != nullptr)
            created = preview.Find("delta")->Find("created_tracks");
        Check(created != nullptr && created->IsArray() &&
                  created->AsArray().size() == 1 &&
                  created->AsArray()[0].Find("alias") != nullptr &&
                  created->AsArray()[0].Find("alias")->AsString() == "T3",
              "a created track comes back with its alias: " +
                  prepared.result_json);
    }
}

// PRD16 -- ROADMAP.md. The presets the catalog offers are the engine's, in
// its order -- the set --export --preset accepts and the window lists -- and
// a preset that contradicts the codec named is refused by name before
// anything is read.
void CheckExportPresets(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    std::vector<std::string> keys;
    for (const ExportPresetDescriptor& preset : Exporter::Presets()) {
        keys.emplace_back(preset.key);
    }
    for (const char* name : {"export_timeline", "export_timelines"}) {
        const auto tool = std::find_if(
            registry.Tools().begin(), registry.Tools().end(),
            [&](const McpTool& entry) { return entry.name == name; });
        Value schema;
        std::string parseError;
        const bool parsed =
            tool != registry.Tools().end() &&
            Value::Parse(tool->input_schema_json, schema, parseError);
        const Value* properties = parsed ? schema.Find("properties") : nullptr;
        const Value* preset = nullptr;
        if (properties != nullptr && std::string(name) == "export_timeline") {
            preset = properties->Find("preset");
        } else if (properties != nullptr && properties->Find("outputs")) {
            const Value* items = properties->Find("outputs")->Find("items");
            preset = items != nullptr && items->Find("properties")
                         ? items->Find("properties")->Find("preset")
                         : nullptr;
        }
        std::vector<std::string> offered;
        if (preset != nullptr && preset->Find("enum") != nullptr) {
            for (const Value& value : preset->Find("enum")->AsArray()) {
                offered.push_back(value.AsString());
            }
        }
        Check(offered == keys,
              std::string(name) + " offers the engine's presets, in its order");
        const Value* description =
            preset != nullptr ? preset->Find("description") : nullptr;
        if (std::string(name) == "export_timeline") {
            for (const std::string& key : keys) {
                Check(description != nullptr && description->AsString().find(
                                                    key) != std::string::npos,
                      "export_timeline.preset says what " + key + " is");
            }
        }
    }

    InMemoryBackend backend(fixture);
    const auto call = [&](const std::string& json) {
        return registry.Call(backend, "export_timeline", ParsedJson(json));
    };
    const std::string head = R"({"timeline_id":")" + fixture.sequence.id +
                             R"(","output_path":"/tmp/prd16.mp4",)";
    McpToolCallOutcome refused =
        call(head + R"("preset":"h264","codec":"hevc"})");
    CheckFailureEnvelope(refused, "an H.264 preset beside codec hevc",
                         "ValidationFailed");
    Check(refused.message.find("export_timeline.preset") != std::string::npos &&
              refused.message.find("contradicts codec 'hevc'") !=
                  std::string::npos,
          "the refusal names the preset and the codec: " + refused.message);
    refused = call(head + R"("preset":"HEVC_MASTER","codec":"h264"})");
    Check(!refused.ok && refused.message.find("hevc_master encodes HEVC") !=
                             std::string::npos,
          "a preset in another case is the same preset: " + refused.message);
    refused = call(head + R"("preset":"prores"})");
    CheckFailureEnvelope(refused, "an unknown preset", "ValidationFailed");
    Check(refused.message.find(Exporter::PresetKeys()) != std::string::npos,
          "an unknown preset is refused with every key: " + refused.message);
    refused = call(R"({"output_path":"/tmp/prd16.mp4"})");
    CheckFailureEnvelope(refused, "an export naming no timeline",
                         "ValidationFailed");
    Check(refused.message.find("'export_timeline.timeline_id' is required") !=
              std::string::npos,
          "export_timeline keeps its timeline_id required: " + refused.message);
}

// PRD17 -- ROADMAP.md. What a series needs of the catalog's schemas: the
// switch of strict timeline mode on the two calls that open a project, the
// choice to activate on the tools that create a timeline, and a timeline to
// read a Resolve timeline into. Strict mode covers every tool that acts on
// one timeline, and nothing else.
void CheckSeriesArguments() {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto declares = [&](const std::string& tool,
                              const std::string& argument) {
        const auto found = std::find_if(
            registry.Tools().begin(), registry.Tools().end(),
            [&](const McpTool& entry) { return entry.name == tool; });
        Value schema;
        std::string parseError;
        return found != registry.Tools().end() &&
               Value::Parse(found->input_schema_json, schema, parseError) &&
               schema.Find("properties") != nullptr &&
               schema.Find("properties")->Find(argument) != nullptr;
    };
    for (const auto& [tool, argument] :
         std::vector<std::pair<std::string, std::string>>{
             {"open_project", "require_timeline"},
             {"create_project", "require_timeline"},
             {"add_timeline", "activate"},
             {"clone_timeline", "activate"},
             {"create_interview_short", "activate"},
             {"import_resolve_timeline", "timeline_id"}}) {
        Check(declares(tool, argument), tool + " declares " + argument);
    }
    for (const char* tool :
         {"insert_clip", "undo", "describe", "propose_grade",
          "prepare_edit_plan", "transcribe_timeline", "import_resolve_timeline",
          "begin_intervention", "export_timeline"}) {
        Check(
            McpToolTargetsOneTimeline(tool),
            std::string(tool) + " acts on one timeline, named in strict mode");
    }
    for (const char* tool :
         {"list_timelines", "add_timeline", "open_project", "transcribe_media",
          "end_intervention", "apply_edit_plan", "get_task"}) {
        Check(!McpToolTargetsOneTimeline(tool),
              std::string(tool) + " names no timeline of its own");
    }
}

// PRD16 -- ROADMAP.md. The exports run as tasks in a server, and stay what
// they are: tools that write the file their caller names. Where a relative
// path lands is decided once, by a table of the arguments that name such a
// file: each names an argument its tool declares, and a server resolves it
// against the project's folder.
void CheckExportTasksAndCallerPaths() {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto find = [&](const std::string& name) {
        return std::find_if(
            registry.Tools().begin(), registry.Tools().end(),
            [&](const McpTool& entry) { return entry.name == name; });
    };
    for (const char* name :
         {"export_timeline", "export_timelines", "render_motion_video"}) {
        const auto tool = find(name);
        Check(
            tool != registry.Tools().end() && tool->background &&
                tool->tool_class == McpToolClass::FileWrite &&
                !tool->writes_analysis_cache &&
                tool->input_schema_json.find("\"async\"") != std::string::npos,
            std::string(name) +
                " runs in the background and still writes a file");
    }

    for (const McpCallerPathArgument& entry : McpCallerPathArguments()) {
        const auto tool = find(entry.tool);
        Value schema;
        std::string parseError;
        const Value* properties =
            tool != registry.Tools().end() &&
                    Value::Parse(tool->input_schema_json, schema, parseError)
                ? schema.Find("properties")
                : nullptr;
        if (properties != nullptr && entry.list != nullptr) {
            const Value* list = properties->Find(entry.list);
            const Value* items =
                list != nullptr ? list->Find("items") : nullptr;
            properties = items != nullptr ? items->Find("properties") : nullptr;
        }
        Check(properties != nullptr &&
                  properties->Find(entry.argument) != nullptr,
              std::string(entry.tool) + " declares the path argument " +
                  entry.argument);
    }

    Value arguments;
    std::string parseError;
    Value::Parse(
        R"({"timeline_id":"T","output_path":"out/../film.mp4","codec":"h264"})",
        arguments, parseError);
    ResolveMcpCallerPaths("export_timeline", "/Volumes/Films", arguments);
    Check(arguments.Find("output_path")->AsString() ==
                  "/Volumes/Films/film.mp4" &&
              arguments.Find("codec")->AsString() == "h264",
          "a relative output path is the project folder's, made normal: " +
              arguments.Dump());
    Value::Parse(R"({"output_path":"/tmp/a.mp4"})", arguments, parseError);
    ResolveMcpCallerPaths("export_timeline", "/Volumes/Films", arguments);
    Check(arguments.Find("output_path")->AsString() == "/tmp/a.mp4",
          "an absolute path is kept as named");
    Value::Parse(R"({"outputs":[{"timeline_id":"A","output_path":"a.mp4"},)"
                 R"({"timeline_id":"B","output_path":"/abs/b.mp4"},"odd"]})",
                 arguments, parseError);
    ResolveMcpCallerPaths("export_timelines", "/Volumes/Films", arguments);
    const Value* outputs = arguments.Find("outputs");
    Check(outputs != nullptr && outputs->AsArray().size() == 3 &&
              outputs->AsArray()[0].Find("output_path")->AsString() ==
                  "/Volumes/Films/a.mp4" &&
              outputs->AsArray()[1].Find("output_path")->AsString() ==
                  "/abs/b.mp4" &&
              outputs->AsArray()[2].AsString() == "odd",
          "each entry of a batch is resolved, the rest left for the tool to "
          "judge: " +
              arguments.Dump());
    Value::Parse(R"({"output_path":"rel.mp4"})", arguments, parseError);
    ResolveMcpCallerPaths("export_timeline", "", arguments);
    ResolveMcpCallerPaths("describe", "/Volumes/Films", arguments);
    Check(arguments.Find("output_path")->AsString() == "rel.mp4",
          "without a project folder, or for a tool the table does not name, "
          "nothing moves");

    // PRD39 -- ROADMAP.md. export_cdl writes the file its caller names, and
    // took a relative one from wherever the server was started.
    Value::Parse(R"({"clip_ids":["A1"],"path":"grades/scene.ccc"})", arguments,
                 parseError);
    ResolveMcpCallerPaths("export_cdl", "/Volumes/Films", arguments);
    Check(arguments.Find("path")->AsString() ==
                  "/Volumes/Films/grades/scene.ccc" &&
              arguments.Find("clip_ids")->Dump() == R"(["A1"])",
          "export_cdl's relative path is the project folder's: " +
              arguments.Dump());
    // DEC19 -- docs/roadmap/PRODUIT-2026-10b.md. check_caption_legibility
    // reads the file an export wrote, as verify_export and check_timeline
    // do, and its relative path is now the project folder's too.
    Value::Parse(R"({"timeline_id":"T","path":"exports/../film.mp4",)"
                 R"("minimum_contrast":{"num":9,"den":2}})",
                 arguments, parseError);
    ResolveMcpCallerPaths("check_caption_legibility", "/Volumes/Films",
                          arguments);
    Check(arguments.Find("path")->AsString() == "/Volumes/Films/film.mp4" &&
              arguments.Find("timeline_id")->AsString() == "T" &&
              arguments.Find("minimum_contrast")->Dump() ==
                  R"({"num":9,"den":2})",
          "check_caption_legibility's relative path is the project folder's: " +
              arguments.Dump());
    for (const char* reader :
         {"verify_export", "check_timeline", "check_caption_legibility"}) {
        Check(std::any_of(McpCallerPathArguments().begin(),
                          McpCallerPathArguments().end(),
                          [&](const McpCallerPathArgument& entry) {
                              return std::string(entry.tool) == reader &&
                                     std::string(entry.argument) == "path";
                          }),
              std::string(reader) + " reads an export's path one way");
    }
    // A tool that requires an absolute path keeps refusing a relative one,
    // and the other tools that read a file read it where it was named.
    for (const char* kept :
         {"export_resolve_xml", "export_motion_after", "after_create_project",
          "after_save_project", "after_render_frame", "after_open_project_copy",
          "create_project", "import_srt", "apply_cdl", "set_sequence_lut"}) {
        Check(std::none_of(McpCallerPathArguments().begin(),
                           McpCallerPathArguments().end(),
                           [&](const McpCallerPathArgument& entry) {
                               return std::string(entry.tool) == kept;
                           }),
              std::string(kept) + " keeps its own reading of a path");
    }
}

// PRD35 -- ROADMAP.md. Every call through the catalog leaves one line in
// the engine log of a process that keeps one -- the tool, how it ended, the
// timeline it named -- and never what it was given nor what it answered.
void CheckEngineLogCalls(const Document& fixture) {
    namespace fs = std::filesystem;
    using mcp_json::Value;
    const fs::path directory =
        fs::temp_directory_path() /
        ("cutmachine_mcp_engine_log_" + std::to_string(::getpid()));
    fs::remove_all(directory);
    const McpToolRegistry registry;
    InMemoryBackend backend(fixture);

    // A process that never started its log -- the CLI's --call-tool, these
    // tests -- goes through the same Call and writes nothing.
    Check(!engine_log::Active(), "a test process keeps no engine log");
    registry.Call(backend, "describe", Value::MakeObject());

    const std::string previousDirectory =
        std::getenv("CUTMACHINE_LOG_DIR") ? std::getenv("CUTMACHINE_LOG_DIR")
                                          : "";
    ::setenv("CUTMACHINE_LOG_DIR", directory.string().c_str(), 1);
    Check(engine_log::StartProcessLog("mcp-stdio", false),
          "the log starts in the folder named");

    const std::string person = "Interview de Marie Dupont";
    const std::string path = "/Users/someone/Clients/Maison X/rush 12.mov";
    Value named;
    std::string parseError;
    Value::Parse(R"({"timeline_id":")" + fixture.sequence.id + R"(","name":")" +
                     person + R"(","time":{"value":5,"rate":25}})",
                 named, parseError);
    const auto marked = registry.Call(backend, "add_marker", named);
    Value refused;
    Value::Parse(R"({"clip_id":")" + path + R"("})", refused, parseError);
    const auto missing = registry.Call(backend, "remove_clip", refused);
    registry.Call(backend, "read /etc/passwd", Value::MakeObject());
    engine_log::StopProcessLog();
    if (previousDirectory.empty())
        ::unsetenv("CUTMACHINE_LOG_DIR");
    else
        ::setenv("CUTMACHINE_LOG_DIR", previousDirectory.c_str(), 1);
    Check(marked.ok && !missing.ok, "the calls ran as they would unlogged");

    std::ifstream input(directory / "cutmachine.log");
    std::vector<std::string> lines;
    std::string text;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
        text += line + "\n";
    }
    Check(lines.size() == 5,
          "start, three calls, stop: " + std::to_string(lines.size()));
    if (lines.size() == 5) {
        Check(lines[1].find(" surface=mcp-stdio call tool=add_marker ok ms=") !=
                      std::string::npos &&
                  lines[1].find(" timeline=" + fixture.sequence.id) !=
                      std::string::npos,
              "a call that succeeded, with the timeline it named: " + lines[1]);
        Check(lines[2].find(" call tool=remove_clip error=" +
                            missing.error_name + " ms=") != std::string::npos &&
                  lines[2].find("timeline=") == std::string::npos,
              "a refused call names its error, and no timeline it did not "
              "name: " +
                  lines[2]);
        Check(lines[3].find(" call tool=? error=UnknownTool ms=") !=
                  std::string::npos,
              "a tool name that is no identifier is not written: " + lines[3]);
    }
    for (const std::string& secret :
         {person, std::string("Marie"), path, std::string("Maison"),
          std::string("passwd"), std::string("\"value\"")}) {
        Check(text.find(secret) == std::string::npos,
              "no argument reaches the log: '" + secret + "'");
    }
    fs::remove_all(directory);
}

// PRD35 -- ROADMAP.md. describe's `history`: a timeline's journal, entry by
// entry, read only when named -- an operation, a labelled batch, a folded
// intervention, an undo, and the two faces of DeleteGap.
void CheckTimelineHistory(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    InMemoryBackend backend(fixture);
    const auto call = [&](McpBackend& on, const std::string& tool,
                          const std::string& json) {
        Value arguments;
        std::string parseError;
        Value::Parse(json, arguments, parseError);
        return registry.Call(on, tool, arguments);
    };
    const auto number = [](const Value* value) -> int64_t {
        int64_t read = -1;
        if (value == nullptr || !value->AsInt64(read)) return -1;
        return read;
    };
    const auto text = [](const Value* value) {
        return value != nullptr && value->IsString() ? value->AsString()
                                                     : std::string("<none>");
    };
    // The answer to only:["history"] is that section alone.
    const auto readHistory = [&](McpBackend& from, Value& history) {
        const McpToolCallOutcome read =
            call(from, "describe", R"({"only":["history"]})");
        Value answer;
        std::string parseError;
        if (!read.ok || !Value::Parse(read.result_json, answer, parseError) ||
            !answer.IsObject() || answer.AsObject().size() != 1 ||
            answer.Find("history") == nullptr)
            return false;
        history = *answer.Find("history");
        return true;
    };

    Value history;
    Check(readHistory(backend, history) &&
              text(history.Find("timeline_id")) == fixture.sequence.id &&
              number(history.Find("total")) == 0 &&
              number(history.Find("applied")) == 0 &&
              history.Find("entries") != nullptr &&
              history.Find("entries")->AsArray().empty(),
          "a timeline never edited has an empty history: " + history.Dump());

    std::string resultJson;
    std::string errorName;
    std::string message;
    // 1. One operation.
    Check(call(backend, "add_marker",
               R"({"name":"Respiration","time":{"value":3,"rate":25}})")
              .ok,
          "a marker is added");
    // 2. A labelled batch, as a tool lays one.
    DocumentMarker first;
    first.name = "A";
    first.time = {4, 25};
    DocumentMarker second;
    second.name = "B";
    second.time = {6, 25};
    Check(backend.ApplyOperation(BatchOperation{"Deux repères",
                                                {AddMarkerOperation{first},
                                                 AddMarkerOperation{second}}},
                                 resultJson, errorName, message),
          "a labelled batch is applied: " + message);
    // 3. A gap closed: a DeleteGap that adds and removes no clip.
    Check(call(backend, "delete_gap",
               R"({"track_id":"01K30000000000000000000002",)"
               R"("gap_start":{"value":15,"rate":25},)"
               R"("gap_duration":{"value":5,"rate":25}})")
              .ok,
          "the gap between the two video clips is closed");
    // 4. A rush inserted from the Source monitor: a DeleteGap too, which
    // adds its clip.
    const std::optional<DeleteGapOperation> insert =
        TimelineSourceEditOperation(
            backend.CurrentDocument(), "01K30000000000000000000001", {0, 25},
            {10, 25}, {25, 25}, "01K30000000000000000000002", true);
    Check(insert.has_value() &&
              backend.ApplyOperation(*insert, resultJson, errorName, message),
          "a rush is inserted at the end of the video track: " + message);
    // 5. An intervention, folded into one labelled entry.
    const McpToolCallOutcome began = call(backend, "begin_intervention", "{}");
    Value token;
    std::string parseError;
    Check(began.ok && Value::Parse(began.result_json, token, parseError) &&
              token.Find("intervention") != nullptr,
          "an intervention begins");
    call(backend, "add_marker", R"({"name":"C","time":{"value":8,"rate":25}})");
    call(backend, "add_marker", R"({"name":"D","time":{"value":9,"rate":25}})");
    Value end = Value::MakeObject();
    if (token.Find("intervention") != nullptr)
        end.Set("intervention", *token.Find("intervention"));
    end.Set("label", Value::MakeString("Agent : repères de respiration"));
    Check(registry.Call(backend, "end_intervention", end).ok,
          "the intervention folds into one entry");
    // 6. Undone.
    Check(call(backend, "undo", "{}").ok, "the intervention is undone");

    Check(readHistory(backend, history), "the history is read");
    const Value* entries = history.Find("entries");
    Check(number(history.Find("total")) == 5 &&
              number(history.Find("applied")) == 4 && entries != nullptr &&
              entries->AsArray().size() == 5,
          "five entries, four in effect: " + history.Dump());
    if (entries != nullptr && entries->AsArray().size() == 5) {
        const auto& list = entries->AsArray();
        const auto is = [&](size_t index, const std::string& type,
                            int64_t operations) {
            return number(list[index].Find("rank")) ==
                       static_cast<int64_t>(index) + 1 &&
                   text(list[index].Find("type")) == type &&
                   number(list[index].Find("operations")) == operations;
        };
        Check(is(0, "AddMarker", 1) && list[0].Find("label") == nullptr &&
                  list[0].Find("undone") == nullptr &&
                  list[0].Find("clips_added") == nullptr,
              "an operation: its type, one operation, no label: " +
                  list[0].Dump());
        Check(
            is(1, "Batch", 2) && text(list[1].Find("label")) == "Deux repères",
            "a labelled batch: its label and its members: " + list[1].Dump());
        Check(is(2, "DeleteGap", 1) &&
                  number(list[2].Find("clips_added")) == 0 &&
                  number(list[2].Find("clips_removed")) == 0,
              "a gap closed adds and removes no clip: " + list[2].Dump());
        Check(is(3, "DeleteGap", 1) &&
                  number(list[3].Find("clips_added")) == 1 &&
                  number(list[3].Find("clips_removed")) == 0,
              "a rush inserted is a DeleteGap that adds its clip: " +
                  list[3].Dump());
        Check(is(4, "Batch", 2) &&
                  text(list[4].Find("label")) ==
                      "Agent : repères de respiration" &&
                  list[4].Find("undone") != nullptr &&
                  list[4].Find("undone")->IsBool() &&
                  list[4].Find("undone")->AsBool(),
              "a folded intervention, undone: " + list[4].Dump());
        for (size_t index = 0; index < 4; ++index)
            Check(list[index].Find("undone") == nullptr,
                  "an entry in effect is not marked undone");
    }
    // Two entries undone keep their places: the next Redo first.
    Check(call(backend, "undo", "{}").ok, "the insert is undone too");
    Check(readHistory(backend, history) &&
              number(history.Find("applied")) == 3 &&
              (entries = history.Find("entries")) != nullptr &&
              entries->AsArray().size() == 5 &&
              number(entries->AsArray()[3].Find("clips_added")) == 1 &&
              entries->AsArray()[3].Find("undone") != nullptr &&
              text(entries->AsArray()[4].Find("label")) ==
                  "Agent : repères de respiration" &&
              entries->AsArray()[4].Find("undone") != nullptr,
          "undone entries stay in the order they were applied: " +
              history.Dump());
    Check(call(backend, "redo", "{}").ok && readHistory(backend, history) &&
              number(history.Find("applied")) == 4 &&
              (entries = history.Find("entries")) != nullptr &&
              entries->AsArray().size() == 5 &&
              entries->AsArray()[3].Find("undone") == nullptr &&
              entries->AsArray()[4].Find("undone") != nullptr,
          "a redo moves the cursor by one entry: " + history.Dump());

    // Never in the default answer; beside the sections named with it.
    Value view;
    const McpToolCallOutcome whole = call(backend, "describe", "{}");
    Check(whole.ok && Value::Parse(whole.result_json, view, parseError) &&
              view.Find("history") == nullptr && view.Find("timeline"),
          "describe's default answer carries no history");
    const McpToolCallOutcome both =
        call(backend, "describe", R"({"only":["sequence","history"]})");
    Check(both.ok && Value::Parse(both.result_json, view, parseError) &&
              view.AsObject().size() == 2 && view.Find("sequence") &&
              view.Find("history") &&
              number(view.Find("history")->Find("total")) == 5,
          "history beside the sections named: " +
              both.result_json.substr(0, 300));
    const McpToolCallOutcome unknown =
        call(backend, "describe", R"({"only":["nope"]})");
    Check(!unknown.ok && unknown.message.find("history") != std::string::npos,
          "a section that does not exist is answered with history among "
          "those there are: " +
              unknown.message);

    // Bounded: an old journal gives its last 200 entries, with the total.
    InMemoryBackend busy(fixture);
    bool applied = true;
    for (int index = 0; index < 205; ++index) {
        DocumentMarker marker;
        marker.name = "M" + std::to_string(index);
        marker.time = {index, 25};
        applied =
            applied && busy.ApplyOperation(AddMarkerOperation{marker},
                                           resultJson, errorName, message);
    }
    const Value* busyEntries = nullptr;
    Check(applied && readHistory(busy, history) &&
              number(history.Find("total")) == 205 &&
              (busyEntries = history.Find("entries")) != nullptr &&
              busyEntries->AsArray().size() == 200 &&
              number(busyEntries->AsArray().front().Find("rank")) == 6 &&
              number(busyEntries->AsArray().back().Find("rank")) == 205,
          "an old journal: the last 200 entries, with the total: " + message);

    // The app's backend reads the journal the window edits.
    Document liveDocument = fixture;
    EditLog liveLog;
    McpLiveBackend live(liveDocument, liveLog);
    Check(call(live, "add_marker",
               R"({"name":"Fenêtre","time":{"value":2,"rate":25}})")
                  .ok &&
              readHistory(live, history) &&
              number(history.Find("total")) == 1 &&
              text(history.Find("timeline_id")) == liveDocument.sequence.id,
          "the app's backend reads the window's journal: " + history.Dump());
}

// PRD40 -- ROADMAP.md. What the first real series of montages (3 October
// 2026) found in the catalog: an intervention of one step lost its label,
// edits answered without a delta, update_sequence wanted every field.
void CheckSeriesFindings(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto call = [&](McpBackend& on, const std::string& tool,
                          const std::string& json) {
        Value arguments;
        std::string parseError;
        Value::Parse(json, arguments, parseError);
        return registry.Call(on, tool, arguments);
    };
    const auto parse = [](const McpToolCallOutcome& outcome) {
        Value view;
        std::string parseError;
        if (!Value::Parse(outcome.result_json, view, parseError) ||
            !view.IsObject())
            view = Value::MakeObject();
        return view;
    };
    const auto text = [](const Value* value) {
        return value != nullptr && value->IsString() ? value->AsString()
                                                     : std::string("<none>");
    };
    const auto number = [](const Value* value) -> int64_t {
        int64_t read = -1;
        if (value == nullptr || !value->AsInt64(read)) return -1;
        return read;
    };

    // 1. An intervention of one step keeps its label, as one of several.
    const auto foldOne = [&](McpBackend& backend, const std::string& label) {
        const McpToolCallOutcome began =
            call(backend, "begin_intervention", "{}");
        const Value token = parse(began);
        if (!began.ok || token.Find("intervention") == nullptr ||
            !call(backend, "add_marker",
                  R"({"name":"Seul","time":{"value":7,"rate":25}})")
                 .ok)
            return false;
        Value end = Value::MakeObject();
        end.Set("intervention", *token.Find("intervention"));
        end.Set("label", Value::MakeString(label));
        const McpToolCallOutcome ended =
            registry.Call(backend, "end_intervention", end);
        const Value answer = parse(ended);
        // A fold changes the history, not the montage: no delta.
        return ended.ok && number(answer.Find("grouped")) == 1 &&
               answer.Find("delta") == nullptr;
    };
    {
        InMemoryBackend backend(fixture);
        const std::string before = backend.CurrentDocument().SaveToString();
        Check(foldOne(backend, "Agent : un repère"),
              "an intervention of one step folds");
        const Value answer =
            parse(call(backend, "describe", R"({"only":["history"]})"));
        const Value* history = answer.Find("history");
        const Value* entries =
            history != nullptr ? history->Find("entries") : nullptr;
        const Value entry = entries != nullptr && entries->IsArray() &&
                                    entries->AsArray().size() == 1
                                ? entries->AsArray().front()
                                : Value::MakeObject();
        Check(text(entry.Find("type")) == "Batch" &&
                  text(entry.Find("label")) == "Agent : un repère" &&
                  number(entry.Find("operations")) == 1,
              "its history entry carries the label, one operation: " +
                  answer.Dump());
        Check(call(backend, "undo", "{}").ok &&
                  backend.CurrentDocument().SaveToString() == before,
              "and one undo reverts it");
    }
    {
        Document liveDocument = fixture;
        EditLog liveLog;
        McpLiveBackend live(liveDocument, liveLog);
        Check(foldOne(live, "Agent : un repère") &&
                  liveLog.AppliedCount() == 1 &&
                  std::holds_alternative<BatchOperation>(
                      liveLog.AppliedEntries().back().op),
              "the app's backend folds one step into the window's journal");
    }

    // 3. Every journaled edit answers with a delta when it changed the
    // timeline -- here over a backend whose own answer carries none.
    {
        InMemoryBackend backend(fixture);
        const Value added =
            parse(call(backend, "add_marker",
                       R"({"name":"Repère","time":{"value":3,"rate":25}})"));
        Check(added.Find("delta") != nullptr,
              "an edit whose backend gave no delta answers with one: " +
                  added.Dump());
        Check(parse(call(backend, "undo", "{}")).Find("delta") != nullptr,
              "and so does its undo");
        Check(parse(call(backend, "timeline_stats", "{}")).Find("delta") ==
                  nullptr,
              "a read has none");
        const McpToolCallOutcome preview =
            call(backend, "shorten_linked_clip",
                 R"({"clip_id":"A1","edge":"Tail","amount":1,)"
                 R"("unit":"Frames","preview":true})");
        Check(preview.ok && parse(preview).Find("delta") == nullptr,
              "nor a preview, which changes nothing: " + preview.message);
    }

    // 6. update_sequence changes the fields given and keeps the others.
    {
        InMemoryBackend backend(fixture);
        const DocumentSequence before = backend.CurrentDocument().sequence;
        const McpToolCallOutcome resized =
            call(backend, "update_sequence", R"({"width":1080,"height":1920})");
        const DocumentSequence& after = backend.CurrentDocument().sequence;
        Check(resized.ok && after.width == 1080 && after.height == 1920 &&
                  after.name == before.name &&
                  after.frame_rate.num == before.frame_rate.num &&
                  after.frame_rate.den == before.frame_rate.den,
              "update_sequence changes the size alone: " + resized.message);
        Check(call(backend, "update_sequence", R"({"name":"Verticale"})").ok &&
                  backend.CurrentDocument().sequence.name == "Verticale" &&
                  backend.CurrentDocument().sequence.width == 1080,
              "or the name alone");
        CheckFailureEnvelope(call(backend, "update_sequence", "{}"),
                             "update_sequence naming no field",
                             "ValidationFailed");
        Check(call(backend, "update_sequence",
                   R"({"name":"Tout","width":1920,"height":1080,)"
                   R"("frame_rate":{"num":25,"den":1}})")
                  .ok,
              "the four fields together are still accepted");
        const auto tool = std::find_if(
            registry.Tools().begin(), registry.Tools().end(),
            [](const McpTool& item) { return item.name == "update_sequence"; });
        Value schema;
        std::string schemaError;
        // The root's `required`: frame_rate's own num and den stay required.
        Check(tool != registry.Tools().end() &&
                  Value::Parse(tool->input_schema_json, schema, schemaError) &&
                  schema.Find("required") == nullptr,
              "update_sequence's schema requires nothing: " + schemaError);
    }
}

}  // namespace

// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The CMX 3600 tools through the
// catalog: the lists of the selected timeline, its markers out and back in
// one undo step. The writer itself is tested by edl_tests.
void CheckEdlTools(const Document& fixture) {
    namespace fs = std::filesystem;
    McpToolRegistry registry;
    InMemoryBackend backend(FixtureWithLibrary(fixture));
    const auto call = [&](const std::string& tool, const std::string& json) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(json, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };
    const fs::path directory =
        fs::temp_directory_path() / ("cutmachine-mcp-edl-" + GenerateUlid());
    fs::create_directories(directory);
    const std::string film = (directory / "film.edl").string();

    const McpToolCallOutcome exported =
        call("export_edl", R"({"path":")" + film + R"("})");
    Check(exported.ok && fs::exists(directory / "film_V1.edl") &&
              exported.result_json.find(R"("clips":2)") != std::string::npos &&
              exported.result_json.find(R"("audio_tracks_not_sent":1)") !=
                  std::string::npos,
          "export_edl writes V1 and names the sound left behind: " +
              exported.message + exported.result_json);
    CheckFailureEnvelope(call("export_edl", R"({"path":")" + film + R"("})"),
                         "export_edl over an existing list", "OutputExists");
    CheckFailureEnvelope(call("export_edl", R"({"path":"film.edl"})"),
                         "export_edl to a relative path", "ValidationFailed");
    CheckFailureEnvelope(
        call("export_edl", R"({"path":")" + (directory / "b.edl").string() +
                               R"(","start_timecode":"01:00:00:99"})"),
        "export_edl from an impossible start", "InvalidExport");

    Check(call("add_marker", R"({"name":"Ici","time":{"value":125,"rate":25},)"
                             R"("color":"#4da3ff"})")
              .ok,
          "a marker to list");
    const std::string marks = (directory / "marks.edl").string();
    const McpToolCallOutcome listed =
        call("export_edl_markers",
             R"({"path":")" + marks + R"(","style":"resolve"})");
    Check(listed.ok && fs::exists(marks) &&
              listed.result_json.find(R"("markers":1)") != std::string::npos,
          "export_edl_markers writes Resolve's style: " + listed.message);
    CheckFailureEnvelope(call("export_edl_markers",
                              R"({"path":")" + (directory / "x.edl").string() +
                                  R"(","style":"avid"})"),
                         "export_edl_markers in a style it does not know",
                         "ValidationFailed");

    const McpToolCallOutcome twice =
        call("import_edl_markers", R"({"path":")" + marks + R"("})");
    Check(
        twice.ok &&
            twice.result_json.find(R"("applied":false)") != std::string::npos &&
            twice.result_json.find(R"("already_present":1)") !=
                std::string::npos &&
            backend.CurrentDocument().sequence.markers.size() == 1,
        "a list of markers the timeline holds adds nothing: " +
            twice.result_json);

    std::ofstream(directory / "review.edl")
        << "* LOC: 01:00:01:00 RED Un\n* LOC: 01:00:02:00 GREEN Deux\n";
    const std::string before = backend.CurrentDocument().SaveToString();
    const McpToolCallOutcome imported =
        call("import_edl_markers",
             R"({"path":")" + (directory / "review.edl").string() + R"("})");
    Check(imported.ok &&
              imported.result_json.find(R"("added":2)") != std::string::npos &&
              backend.CurrentDocument().sequence.markers.size() == 3,
          "import_edl_markers adds two markers: " + imported.message +
              imported.result_json);
    Check(call("undo", "{}").ok &&
              backend.CurrentDocument().SaveToString() == before,
          "one undo takes the whole import back");
    std::ofstream(directory / "orange.edl") << "* LOC: 01:00:01:00 ORANGE Un\n";
    const McpToolCallOutcome orange =
        call("import_edl_markers",
             R"({"path":")" + (directory / "orange.edl").string() + R"("})");
    Check(!orange.ok && orange.message.find("ORANGE") != std::string::npos,
          "an unknown colour is refused by name: " + orange.message);
    fs::remove_all(directory);
}

// MOTIF2, MOTIF3, MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. Why an edit is
// made reaches the journal from every path an edit takes: a call's
// `motif`, a rule tool's own, a plan's steps, an intervention folded; and
// list_motifs reads it back grouped.
void CheckMotifs(const Document& fixture) {
    using mcp_json::Value;
    const McpToolRegistry registry;
    const auto call = [&](McpBackend& on, const std::string& tool,
                          const std::string& json) {
        return registry.Call(on, tool, ParsedJson(json));
    };
    const auto lastMotifs = [](InMemoryBackend& backend) {
        const auto& entries = backend.Log().AppliedEntries();
        return entries.empty() ? std::vector<EntryMotif>{}
                               : entries.back().motifs;
    };

    {
        InMemoryBackend backend(fixture);
        const McpToolCallOutcome marked =
            call(backend, "add_marker",
                 R"({"name":"Coupe ici","time":{"value":3,"rate":25},)"
                 R"("motif":{"reason":"brief","note":"le brief veut 20 s",)"
                 R"("author":"person"}})");
        Check(marked.ok, "an edit takes a motif: " + marked.message);
        const std::vector<EntryMotif> motifs = lastMotifs(backend);
        Check(motifs.size() == 1 && motifs[0].first == 0 &&
                  motifs[0].count == 1 &&
                  motifs[0].motif.reason == MotifReason::Brief &&
                  motifs[0].motif.author == MotifAuthor::Person &&
                  motifs[0].motif.note == "le brief veut 20 s" &&
                  motifs[0].motif.rule.empty(),
              "the motif rides on the entry the call wrote");
        Check(backend.CurrentEditMotifs().motif == std::nullopt,
              "the call's motif is cleared once it has ended");
        Ulid markerId;
        for (const DocumentMarker& marker :
             backend.CurrentDocument().sequence.markers) {
            if (marker.name == "Coupe ici") markerId = marker.id;
        }
        Check(markerId.size() == 26, "the marker was laid");

        Check(call(backend, "add_marker",
                   R"({"name":"Sans motif","time":{"value":4,"rate":25}})")
                      .ok &&
                  lastMotifs(backend).empty(),
              "an edit without a motif writes none");
        // A marker named by a prefix of its ID, resolved to the ID. IDs made
        // in the same millisecond share most of their digits: the shortest
        // prefix that names it alone.
        std::string prefix = markerId;
        {
            const IdResolver ids(backend.CurrentDocument());
            for (size_t length = 12; length < markerId.size(); ++length) {
                Ulid resolved;
                std::string ignored;
                if (ids.Resolve("marker", markerId.substr(0, length), resolved,
                                ignored) &&
                    resolved == markerId) {
                    prefix = markerId.substr(0, length);
                    break;
                }
            }
        }
        const McpToolCallOutcome updated =
            call(backend, "update_marker",
                 R"({"marker_id":")" + markerId +
                     R"(","name":"Coupé","time":{"value":3,"rate":25},)"
                     R"("color":"#f5c542","category":"note",)"
                     R"("motif":{"reason":"marker",)"
                     R"("marker_id":")" +
                     prefix + R"("}})");
        Check(updated.ok && !lastMotifs(backend).empty() &&
                  lastMotifs(backend)[0].motif.marker_id == markerId,
              "a motif's marker is resolved to its full ID: " +
                  updated.result_json);

        CheckFailureEnvelope(
            call(backend, "describe", R"({"motif":{"reason":"brief"}})"),
            "a read carries no motif", "ValidationFailed");
        CheckFailureEnvelope(
            call(backend, "add_marker",
                 R"({"name":"X","time":{"value":5,"rate":25},)"
                 R"("motif":{"reason":"brief","rule":"tighten_pauses"}})"),
            "a caller does not name a rule", "ValidationFailed");
        CheckFailureEnvelope(call(backend, "add_marker",
                                  R"({"name":"X","time":{"value":5,"rate":25},)"
                                  R"("motif":{"reason":"envie"}})"),
                             "a reason outside the vocabulary is refused",
                             "ValidationFailed");
        CheckFailureEnvelope(call(backend, "add_marker",
                                  R"({"name":"X","time":{"value":5,"rate":25},)"
                                  R"("motif":{"reason":"other"}})"),
                             "'other' needs a note", "ValidationFailed");
        CheckFailureEnvelope(
            call(backend, "undo", R"({"motif":{"reason":"brief"}})"),
            "undo moves entries and writes no motif", "ValidationFailed");

        // An intervention folded: each call's motif on its members, the
        // end's motif on the rest.
        const McpToolCallOutcome began =
            call(backend, "begin_intervention", "{}");
        Value token;
        std::string parseError;
        Check(began.ok && Value::Parse(began.result_json, token, parseError),
              "an intervention begins");
        call(backend, "add_marker",
             R"({"name":"A","time":{"value":8,"rate":25},)"
             R"("motif":{"reason":"rhythm"}})");
        call(backend, "add_marker",
             R"({"name":"B","time":{"value":9,"rate":25}})");
        Value end = Value::MakeObject();
        end.Set("intervention", *token.Find("intervention"));
        end.Set("label", Value::MakeString("Deux repères"));
        end.Set("motif", ParsedJson(R"({"reason":"brief","note":"v2"})"));
        Check(registry.Call(backend, "end_intervention", end).ok,
              "the intervention folds under a motif");
        const std::vector<EntryMotif> folded = lastMotifs(backend);
        Check(folded.size() == 2 && folded[0].first == 0 &&
                  folded[0].count == 1 &&
                  folded[0].motif.reason == MotifReason::Rhythm &&
                  folded[1].first == 1 && folded[1].count == 1 &&
                  folded[1].motif.reason == MotifReason::Brief,
              "a fold keeps each call's motif and fills the rest");

        // The journal round-trips with its motifs.
        EditLog reread;
        EditError error = EditError::None;
        std::string message;
        Check(EditLog::Deserialize(backend.Log().Serialize(), reread, error,
                                   message) &&
                  reread.Serialize() == backend.Log().Serialize(),
              "a journal with motifs reads back to its own bytes: " + message);

        // list_motifs.
        const McpToolCallOutcome listed = call(backend, "list_motifs", "{}");
        Value answer;
        Check(listed.ok && Value::Parse(listed.result_json, answer, parseError),
              "list_motifs answers: " + listed.message);
        const Value* groups = answer.Find("groups");
        int64_t without = -1;
        Check(groups != nullptr && groups->AsArray().size() == 3 &&
                  answer.Find("operations_without_motif") != nullptr &&
                  answer.Find("operations_without_motif")->AsInt64(without) &&
                  without == 1,
              "three groups (brief, marker, rhythm) and one edit nobody "
              "explained: " +
                  listed.result_json);
        const McpToolCallOutcome briefOnly =
            call(backend, "list_motifs", R"({"reason":"brief"})");
        Value briefAnswer;
        Check(
            briefOnly.ok &&
                Value::Parse(briefOnly.result_json, briefAnswer, parseError) &&
                briefAnswer.Find("groups")->AsArray().size() == 1 &&
                briefAnswer.Find("groups")
                        ->AsArray()[0]
                        .Find("entries")
                        ->AsArray()
                        .size() == 2,
            "list_motifs filters on a reason: " + briefOnly.result_json);

        // A plan's steps say why, step by step.
        const McpToolCallOutcome prepared =
            call(backend, "prepare_edit_plan",
                 R"({"label":"Plan","steps":[)"
                 R"({"tool":"add_marker","arguments":{"name":"P1",)"
                 R"("time":{"value":11,"rate":25},)"
                 R"("motif":{"reason":"cutaway"}}},)"
                 R"({"tool":"add_marker","arguments":{"name":"P2",)"
                 R"("time":{"value":12,"rate":25}}}]})");
        Value plan;
        Check(
            prepared.ok && Value::Parse(prepared.result_json, plan, parseError),
            "a plan with a step's motif is prepared: " + prepared.message);
        Value apply = Value::MakeObject();
        if (plan.Find("plan_id") != nullptr)
            apply.Set("plan_id", *plan.Find("plan_id"));
        apply.Set("motif", ParsedJson(R"({"reason":"selection"})"));
        Check(registry.Call(backend, "apply_edit_plan", apply).ok,
              "the plan is applied");
        const std::vector<EntryMotif> planned = lastMotifs(backend);
        Check(planned.size() == 2 &&
                  planned[0].motif.reason == MotifReason::Cutaway &&
                  planned[1].motif.reason == MotifReason::Selection,
              "the step's motif on its member, the call's on the other");
        Check(!backend.CurrentEditMotifs().motif &&
                  backend.CurrentEditMotifs().members.empty(),
              "apply_edit_plan leaves no motif behind");
        // Undo and redo carry the motifs with their entry.
        Check(call(backend, "undo", "{}").ok &&
                  backend.Log().UndoneEntries().back().motifs.size() == 2 &&
                  call(backend, "redo", "{}").ok &&
                  lastMotifs(backend).size() == 2,
              "undo and redo move an entry with its motifs");
    }

    // MOTIF3 -- a rule tool records its rule and the thresholds it cut with.
    {
        Document pauses;
        pauses.sources = {
            {"01K30000000000000000000001", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture;
        picture.id = "01K30000000000000000000010";
        picture.source_id = "01K30000000000000000000001";
        picture.source_in = {0, 25};
        picture.duration = {75, 25};
        picture.timeline_in = {0, 25};
        pauses.sequence.tracks = {
            {"01K30000000000000000000013", "video", 0, {picture}}};
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000001";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        for (int index = 0; index < 150; ++index)
            envelope.levels.push_back(index >= 50 && index < 100 ? 20000
                                                                 : 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        InMemoryBackend refused(pauses);
        refused.SetSourceSpeechOnset(envelope);
        CheckFailureEnvelope(call(refused, "tighten_pauses",
                                  R"({"clip_id":"01K30000000000000000000010",)"
                                  R"("motif":{"reason":"brief"}})"),
                             "a rule tool keeps its own reason",
                             "ValidationFailed");
        Check(refused.Log().AppliedCount() == 0,
              "and nothing was applied before the refusal");

        InMemoryBackend backend(pauses);
        backend.SetSourceSpeechOnset(envelope);
        const McpToolCallOutcome tightened =
            call(backend, "tighten_pauses",
                 R"({"clip_id":"01K30000000000000000000010","min_gap_ms":300,)"
                 R"("motif":{"note":"rythme du reel"}})");
        Check(tightened.ok, "tighten_pauses applies: " + tightened.message);
        const std::vector<EntryMotif> motifs = lastMotifs(backend);
        int64_t threshold = -1;
        Check(
            motifs.size() == 1 &&
                motifs[0].motif.reason == MotifReason::Pause &&
                motifs[0].motif.rule == "tighten_pauses" &&
                motifs[0].motif.note == "rythme du reel" &&
                motifs[0].motif.params.Find("min_gap_ms") != nullptr &&
                motifs[0].motif.params.Find("min_gap_ms")->AsInt64(threshold) &&
                threshold == 300 &&
                motifs[0].motif.params.Find("keep_frames") != nullptr,
            "the pauses are journaled as the rule's, thresholds and "
            "defaults included, the caller's note kept");
        const McpToolCallOutcome listed =
            call(backend, "list_motifs", R"({"rule":"tighten_pauses"})");
        Check(listed.ok &&
                  listed.result_json.find("\"source_ranges\"") !=
                      std::string::npos &&
                  listed.result_json.find("\"min_gap_ms\":300") !=
                      std::string::npos,
              "list_motifs names the rule's parameters and the ranges it "
              "cut: " +
                  listed.result_json);
    }
}

// ---------------------------------------------------------------------
// DC1 -- docs/roadmap/DECOUPAGE-2026-10.md. Dispatchers the suite reached
// only with wrong arguments, measured with llvm-cov on 27 October 2026:
// each one's successful path and the refusals it decides itself, so that
// splitting McpTools.cc cannot lose one without a failure here.
// ---------------------------------------------------------------------

McpToolCallOutcome CallWithJson(McpToolRegistry& registry, McpBackend& backend,
                                const std::string& tool,
                                const std::string& json) {
    mcp_json::Value arguments;
    std::string parseError;
    Check(mcp_json::Value::Parse(json, arguments, parseError),
          tool + " arguments parse: " + parseError);
    return registry.Call(backend, tool, arguments);
}

void CheckInspectCutDispatch(const Document& fixture) {
    // The render is the backend's; the dispatcher owns the cut it plans,
    // the settings it passes and the picture it hands back.
    class InspectBackend : public InMemoryBackend {
    public:
        using InMemoryBackend::InMemoryBackend;
        std::vector<CutInspectionPlan> plans;
        CutInspectionRenderSettings settings;
        bool fail = false;
        bool InspectCut(const CutInspectionPlan& plan,
                        const CutInspectionRenderSettings& asked,
                        CutInspectionResult& result,
                        std::string& message) override {
            plans.push_back(plan);
            settings = asked;
            if (fail) {
                message = "the decoder is gone";
                return false;
            }
            result.jpeg = "fake-jpeg";
            return true;
        }
    };
    McpToolRegistry registry;
    InspectBackend backend(fixture);
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "inspect_cut", json);
    };

    const McpToolCallOutcome head =
        call(R"({"clip_id":"01K30000000000000000000004","images":4,)"
             R"("transcribe":true,"language":"fr","band":true})");
    Check(head.ok && backend.plans.size() == 1 && backend.plans[0].cut == 20 &&
              backend.plans[0].shown.size() == 4 &&
              backend.settings.transcribe &&
              backend.settings.language == "fr" && backend.settings.cut_band,
          "inspect_cut plans the clip's head and passes its settings: " +
              head.message);
    Check(head.image_base64 == "ZmFrZS1qcGVn" &&
              head.image_mime == "image/jpeg" &&
              ParsedJson(head.result_json).IsObject(),
          "inspect_cut returns the backend's picture beside its report: " +
              head.result_json);

    Check(
        call(R"({"clip_id":"01K30000000000000000000003","at_tail":true})").ok &&
            backend.plans.size() == 2 && backend.plans[1].cut == 15,
        "at_tail inspects the clip's end");
    Check(call(R"({"position":{"value":15,"rate":25}})").ok &&
              backend.plans.size() == 3 && backend.plans[2].cut == 15,
          "a position names the cut by its time");

    CheckFailureEnvelope(
        call(R"({"position":{"value":15,"rate":25},"at_tail":true})"),
        "at_tail beside a position", "ValidationFailed");
    CheckFailureEnvelope(call(R"({})"), "neither a clip nor a position",
                         "ValidationFailed");
    Check(backend.plans.size() == 3, "a refused request renders nothing");
    backend.fail = true;
    CheckFailureEnvelope(call(R"({"clip_id":"01K30000000000000000000004"})"),
                         "a render that fails", "IoError");
}

void CheckReviewCutsDispatch(const Document& fixture) {
    // Two pictures that meet at frame 15, and a caption track whose clip the
    // review has nothing to say about.
    Document montage = fixture;
    montage.sequence.frame_rate = {25, 1};
    montage.sequence.width = 1280;
    montage.sequence.height = 720;
    montage.sequence.tracks[0].clips[1].timeline_in = {15, 25};
    DocumentClip caption;
    caption.id = "01K3C000000000000000000001";
    caption.source_id = "01K30000000000000000000001";
    caption.source_in = {0, 25};
    caption.duration = {10, 25};
    caption.timeline_in = {5, 25};
    montage.sequence.tracks.push_back(
        {"01K3C000000000000000000002", "caption", 2, {caption}});

    // Measurable windows: a quiet mix and still planes, so the review
    // succeeds without an alert.
    class ReviewBackend : public InMemoryBackend {
    public:
        using InMemoryBackend::InMemoryBackend;
        std::vector<int64_t> cuts;
        bool fail = false;
        int revisions = 0;
        bool moving = false;
        bool InspectCut(const CutInspectionPlan& plan,
                        const CutInspectionRenderSettings& settings,
                        CutInspectionResult& result,
                        std::string& message) override {
            cuts.push_back(plan.cut);
            if (fail) {
                message = "the decoder is gone";
                return false;
            }
            result.audio.sample_rate = 48000;
            // Interleaved stereo at 48 kHz, 1920 samples a frame at 25.
            result.samples.assign(
                static_cast<size_t>(plan.end - plan.first) * 1920 * 2, 0.003f);
            result.analysis_width = 64;
            result.analysis_height = 36;
            result.analysis_planes.assign(
                plan.shown.size(), std::vector<uint8_t>(size_t{64} * 36, 128));
            result.words.expected_unavailable = "no transcript";
            result.jpeg = settings.cut_band ? "band" : "";
            return true;
        }
        bool Revision(std::string& revision, std::string& message) override {
            if (!moving) return McpBackend::Revision(revision, message);
            revision = "r" + std::to_string(++revisions);
            return true;
        }
    };
    McpToolRegistry registry;
    ReviewBackend backend(montage);
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "review_cuts", json);
    };

    const McpToolCallOutcome byTime =
        call(R"({"positions":[{"value":15,"rate":25}],"max_bands":0})");
    Check(byTime.ok && backend.cuts == std::vector<int64_t>{15} &&
              byTime.image_base64.empty() &&
              byTime.result_json.find("\"reviewed\":1") != std::string::npos &&
              byTime.result_json.find("\"bands\":[]") != std::string::npos,
          "review_cuts reviews the junction a position names, as text when "
          "no band is asked: " +
              byTime.message + byTime.result_json);
    backend.cuts.clear();
    Check(call(R"({"clip_ids":["01K30000000000000000000004"],"max_bands":0})")
                  .ok &&
              backend.cuts == std::vector<int64_t>{15},
          "a clip names the junction at its head");

    backend.cuts.clear();
    CheckFailureEnvelope(call(R"({"positions":[{"value":1,"rate":50}]})"),
                         "a position between two frames", "ValidationFailed");
    CheckFailureEnvelope(call(R"({"positions":{"value":15,"rate":25}})"),
                         "positions that are not an array", "ValidationFailed");
    const McpToolCallOutcome onCaption =
        call(R"({"clip_ids":["01K3C000000000000000000001"]})");
    CheckFailureEnvelope(onCaption, "a clip on a caption track",
                         "ValidationFailed");
    Check(
        onCaption.message.find("no video or audio track") != std::string::npos,
        "the refusal says where the clip is not: " + onCaption.message);
    CheckFailureEnvelope(call(R"({"positions":[{"value":10,"rate":25}]})"),
                         "a time where no junction is", "ValidationFailed");
    Check(backend.cuts.empty(), "a refused request renders nothing");

    backend.fail = true;
    CheckFailureEnvelope(call(R"({"positions":[{"value":15,"rate":25}]})"),
                         "a window that cannot be rendered", "IoError");
    backend.fail = false;
    backend.moving = true;
    const McpToolCallOutcome raced =
        call(R"({"positions":[{"value":15,"rate":25}],"max_bands":0})");
    CheckFailureEnvelope(raced, "a project that moves during the review",
                         "ValidationFailed");
    Check(
        raced.message.find("changed during the review") != std::string::npos,
        "the refusal says the report would mix two montages: " + raced.message);
}

void CheckMusicStructureDispatch() {
    // 16 s at 120 BPM whose low band comes in at 8 s: the shape of the
    // engine's own case (beat_detection_tests.cc), as the beat cache holds
    // it, without decoding a file.
    constexpr uint32_t kRate = 44100;
    constexpr uint32_t kHop = 512;
    const Ulid music = "01K3S000000000000000000001";
    Document document;
    LibraryMedia media;
    media.id = music;
    media.path = "music/drop.wav";
    media.filename = "drop.wav";
    media.rate = {25, 1};
    media.duration = {400, 25};
    media.metadata_complete = true;
    media.has_audio = true;
    media.audio_rate = kRate;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{music, media.path, media.rate, media.duration}};

    class BeatsBackend : public InMemoryBackend {
    public:
        using InMemoryBackend::InMemoryBackend;
        BeatDetectionResult beats;
        bool ReadSourceAudioBeats(const Ulid&, BeatDetectionResult& result,
                                  std::string&) override {
            result = beats;
            return true;
        }
    };
    BeatsBackend backend(document);
    for (int64_t beat = 0; beat < 32; ++beat)
        backend.beats.onsets.push_back({{beat * kRate / 2, kRate}, 1.0f});
    AudioBandEnvelope bands;
    bands.sample_rate = kRate;
    bands.hop_size = kHop;
    bands.window_size = 1024;
    const size_t frames = 16 * kRate / kHop;
    for (size_t frame = 0; frame < frames; ++frame) {
        const bool bass = (frame * kHop + 512) >= size_t{8} * kRate;
        bands.low_centi_db.push_back(bass ? 0 : -4000);
        bands.high_centi_db.push_back(-1000);
        bands.full_centi_db.push_back(bass ? 0 : -1500);
    }
    backend.beats.bands = bands;
    McpToolRegistry registry;
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "list_music_structure", json);
    };

    const McpToolCallOutcome read =
        call(R"({"media_id":"01K3S000000000000000000001","beats_per_bar":4,)"
             R"("bar_origin":{"value":0,"rate":44100},"max_beats":8})");
    const mcp_json::Value answer = ParsedJson(read.result_json);
    const mcp_json::Value* changes = answer.Find("changes");
    const mcp_json::Value* rows = answer.Find("beats");
    const mcp_json::Value* columns = answer.Find("beat_columns");
    const mcp_json::Value* next = answer.Find("next_source_in");
    int64_t sample = -1;
    const bool rise =
        changes != nullptr && changes->IsArray() &&
        changes->AsArray().size() == 1 &&
        changes->AsArray()[0].Find("kind") != nullptr &&
        changes->AsArray()[0].Find("kind")->AsString() == "rise" &&
        changes->AsArray()[0].Find("bar") != nullptr &&
        changes->AsArray()[0].Find("source_time") != nullptr &&
        changes->AsArray()[0]
            .Find("source_time")
            ->Find("value")
            ->AsInt64(sample) &&
        std::llabs(sample - 8 * int64_t{kRate}) < kRate / 20;
    Check(read.ok && rise,
          "list_music_structure names the bass coming in at 8 s, on its "
          "bar: " +
              read.message + read.result_json);
    Check(rows != nullptr && rows->IsArray() && rows->AsArray().size() == 8 &&
              columns != nullptr && columns->AsArray().size() == 7 &&
              next != nullptr,
          "the beats stop at max_beats, bars declared, and say where to "
          "read on: " +
              read.result_json);
    const McpToolCallOutcome unbarred =
        call(R"({"media_id":"01K3S000000000000000000001","max_beats":512})");
    const mcp_json::Value plain = ParsedJson(unbarred.result_json);
    Check(unbarred.ok && plain.Find("beat_columns") != nullptr &&
              plain.Find("beat_columns")->AsArray().size() == 5 &&
              plain.Find("next_source_in") == nullptr,
          "without a declared origin no beat carries a bar, and all fit: " +
              unbarred.result_json);

    CheckFailureEnvelope(
        call(R"({"media_id":"01K3S000000000000000000001","max_beats":513})"),
        "more beats than a page", "ValidationFailed");
    backend.beats.bands.reset();
    const McpToolCallOutcome legacy =
        call(R"({"media_id":"01K3S000000000000000000001"})");
    CheckFailureEnvelope(legacy, "a beat cache from before the bands",
                         "ValidationFailed");
    Check(legacy.message.find("run analyze_audio_beats again") !=
              std::string::npos,
          "the refusal says what to run: " + legacy.message);
}

void CheckConformColorManagementDispatch() {
    Document hlg = FirstClipFixture();
    LibraryMedia& rush = hlg.library[0];
    rush.metadata_complete = true;
    rush.has_video = true;
    rush.color_transfer = "arib-std-b67";
    rush.color_primaries = "bt2020";
    rush.color_space = "bt2020nc";
    rush.color_range = "tv";
    McpToolRegistry registry;
    InMemoryBackend backend(hlg);
    const std::string before = backend.CurrentDocument().SaveToString();
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "conform_color_management",
                            json);
    };
    const auto field = [](const mcp_json::Value& value, const char* name) {
        const mcp_json::Value* found = value.Find(name);
        return found != nullptr && found->IsString() ? found->AsString()
                                                     : std::string();
    };

    const McpToolCallOutcome preview = call(R"({"preview":true})");
    const mcp_json::Value proposed = ParsedJson(preview.result_json);
    const mcp_json::Value* settings = proposed.Find("proposed");
    Check(preview.ok && settings != nullptr &&
              field(*settings, "input_transfer") == "hlg" &&
              field(*settings, "input_gamut") == "rec2020" &&
              proposed.Find("applied") != nullptr &&
              !proposed.Find("applied")->AsBool() &&
              backend.CurrentDocument().SaveToString() == before,
          "a preview proposes HLG from the rush's tags and changes nothing: " +
              preview.result_json);

    const McpToolCallOutcome applied = call("{}");
    const ColorManagementSettings& now =
        backend.CurrentDocument().color_management;
    Check(applied.ok && now.enabled && now.input_transfer == "hlg" &&
              now.input_gamut == "rec2020" &&
              applied.result_json.find("\"applied\":true") != std::string::npos,
          "conform_color_management sets the project's input: " +
              applied.message + applied.result_json);
    const McpToolCallOutcome again = call(R"({"preview":true})");
    Check(again.ok && again.result_json.find("\"already_conformed\":true") !=
                          std::string::npos,
          "asked again, it says the project already follows its rushes: " +
              again.result_json);
    std::string undone;
    std::string errorName;
    std::string message;
    Check(backend.Undo(undone, errorName, message) &&
              backend.CurrentDocument().SaveToString() == before,
          "one undo gives the project its colour back");

    Document untagged = FirstClipFixture();
    untagged.library[0].metadata_complete = true;
    untagged.library[0].has_video = true;
    InMemoryBackend unknown(untagged);
    const McpToolCallOutcome undecided =
        CallWithJson(registry, unknown, "conform_color_management", "{}");
    Check(undecided.ok &&
              undecided.result_json.find("\"determined\":false") !=
                  std::string::npos &&
              unknown.Log().AppliedCount() == 0,
          "untagged rushes decide nothing and apply nothing: " +
              undecided.result_json);
}

void CheckSplitMulticamGroupDispatch(const Document& fixture) {
    Document multicam = fixture;
    const Ulid wide = "01K30000000000000000000003";
    const Ulid close = "01K3M000000000000000000021";
    multicam.sequence.tracks.push_back({"01K3T000000000000000000020",
                                        "video",
                                        2,
                                        {{close,
                                          "01K30000000000000000000001",
                                          {300, 25},
                                          {10, 25},
                                          {5, 25}}}});
    McpToolRegistry registry;
    InMemoryBackend backend(multicam);
    const auto call = [&](const std::string& tool, const std::string& json) {
        return CallWithJson(registry, backend, tool, json);
    };
    const McpToolCallOutcome added = call(
        "add_multicam_group", R"({"angles":[{"clip_id":")" + wide +
                                  R"("},{"clip_id":")" + close + R"("}]})");
    const mcp_json::Value addedAnswer = ParsedJson(added.result_json);
    const mcp_json::Value* group = addedAnswer.Find("group_id");
    Check(added.ok && group != nullptr, "a group to split: " + added.message);
    if (group == nullptr) return;
    const std::string groupId = group->AsString();
    const MulticamGroup* stored =
        backend.CurrentDocument().FindMulticamGroup(groupId);
    const Ulid closeAngle = stored != nullptr && stored->angles.size() == 2
                                ? stored->angles[1].id
                                : Ulid();
    const std::string before = backend.CurrentDocument().SaveToString();

    const McpToolCallOutcome split =
        call("split_multicam_group",
             R"({"group_id":")" + groupId +
                 R"(","timeline_position":{"value":10,"rate":25},)"
                 R"("active_angle_id":")" +
                 closeAngle + R"("})");
    const mcp_json::Value answer = ParsedJson(split.result_json);
    const mcp_json::Value* created = answer.Find("group_id");
    const mcp_json::Value* angles = answer.Find("angle_ids");
    const mcp_json::Value* active = answer.Find("active_angle_id");
    Check(split.ok && created != nullptr && created->AsString() != groupId &&
              angles != nullptr && angles->AsArray().size() == 2 &&
              active != nullptr && !active->AsString().empty() &&
              backend.CurrentDocument().sequence.multicam_groups.size() == 2 &&
              answer.Find("delta") != nullptr,
          "split_multicam_group cuts the group in two and names the new "
          "one, its angles and the angle it shows: " +
              split.message + split.result_json);
    std::string undone;
    std::string errorName;
    std::string message;
    Check(backend.Undo(undone, errorName, message) &&
              backend.CurrentDocument().SaveToString() == before,
          "one undo joins the group again");
    CheckFailureEnvelope(
        call("split_multicam_group",
             R"({"group_id":")" + groupId +
                 R"(","timeline_position":{"value":40,"rate":25}})"),
        "a position outside the group");
    CheckFailureEnvelope(
        call("split_multicam_group",
             R"({"group_id":")" + groupId +
                 R"(","timeline_position":{"value":10,"rate":25},)"
                 R"("angle":"A"})"),
        "an unknown argument", "ValidationFailed");
    Check(backend.CurrentDocument().SaveToString() == before,
          "a refused split leaves the group whole");
}

void CheckMeasureAudioOffsetDispatch() {
    class OffsetBackend : public InMemoryBackend {
    public:
        using InMemoryBackend::InMemoryBackend;
        Ulid reference;
        Ulid other;
        AudioOffsetChannels channels;
        int measured = 0;
        bool MeasureAudioOffset(const Ulid& referenceId, const Ulid& otherId,
                                const AudioOffsetChannels& asked,
                                std::string& resultJson, std::string&,
                                std::string&) override {
            reference = referenceId;
            other = otherId;
            channels = asked;
            ++measured;
            resultJson = R"({"ok":true,"offset":{"value":0,"rate":48000}})";
            return true;
        }
    };
    McpToolRegistry registry;
    OffsetBackend backend(FirstClipFixture());
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "measure_audio_offset", json);
    };
    Check(call(R"({"reference_id":"M1","other_id":"M1"})").ok &&
              backend.reference == kFirstRush && backend.other == kFirstRush &&
              backend.channels.reference == AudioOffsetChannel::Mix &&
              backend.channels.other == AudioOffsetChannel::Mix,
          "measure_audio_offset resolves its media and hears the mix by "
          "default");
    Check(call(R"({"reference_id":"M1","other_id":"M1",)"
               R"("reference_channel":"left","other_channel":"right"})")
                  .ok &&
              backend.channels.reference == AudioOffsetChannel::Left &&
              backend.channels.other == AudioOffsetChannel::Right,
          "a channel named on each side reaches the measurement");
    const McpToolCallOutcome middle =
        call(R"({"reference_id":"M1","other_id":"M1",)"
             R"("reference_channel":"middle"})");
    CheckFailureEnvelope(middle, "a channel that does not exist",
                         "ValidationFailed");
    Check(middle.message.find("mix, left or right") != std::string::npos &&
              backend.measured == 2,
          "the refusal names the channels, before any measurement: " +
              middle.message);
}

void CheckAlignWordsDispatch() {
    class AlignBackend : public InMemoryBackend {
    public:
        using InMemoryBackend::InMemoryBackend;
        std::vector<Ulid> media;
        std::string texts;
        int aligned = 0;
        bool AlignSourceWords(const std::vector<Ulid>& mediaIds,
                              const std::string& textsJson,
                              std::string& resultJson, std::string&) override {
            media = mediaIds;
            texts = textsJson;
            ++aligned;
            resultJson = R"({"ok":true})";
            return true;
        }
    };
    McpToolRegistry registry;
    AlignBackend backend(FirstClipFixture());
    const auto call = [&](const std::string& json) {
        return CallWithJson(registry, backend, "align_words", json);
    };
    // The texts are keyed like every other id an agent writes, and reach
    // the engine under the rush's ULID (Whisper's long-form drift was fixed
    // by passing the words said).
    Check(call(R"({"media_ids":["M1"],"texts":{"M1":"Bonjour à tous."}})").ok &&
              backend.media == std::vector<Ulid>{kFirstRush} &&
              backend.texts ==
                  std::string(R"({")") + kFirstRush + R"(":"Bonjour à tous."})",
          "align_words resolves the keys of its texts: " + backend.texts);
    CheckFailureEnvelope(call(R"({"media_ids":["M1"],"texts":{"M1":3}})"),
                         "a text that is not a string", "ValidationFailed");
    CheckFailureEnvelope(call(R"({"media_ids":["M1"],"texts":{"M9":"x"}})"),
                         "a text for no known media", "ValidationFailed");
    Check(backend.aligned == 1, "a refused text aligns nothing");
}

void CheckCaptionLegibilityDispatch(const Document& fixture) {
    McpToolRegistry registry;
    InMemoryBackend backend(fixture);
    const std::string timeline = backend.CurrentDocument().sequence.id;
    const auto call = [&](const std::string& contrast) {
        return CallWithJson(registry, backend, "check_caption_legibility",
                            R"({"timeline_id":")" + timeline +
                                R"(","path":"/tmp/none.mp4",)"
                                R"("minimum_contrast":)" +
                                contrast + "}");
    };
    // The check reads the exported file, which this test has not made: a
    // contrast that passes is one refused for the file, not for itself.
    const McpToolCallOutcome body = call(R"({"num":9,"den":2})");
    Check(!body.ok && body.error_name == "IoError" &&
              body.message.find("cannot be opened") != std::string::npos,
          "the WCAG level of body text is accepted as a fraction: " +
              body.message);
    CheckFailureEnvelope(call(R"({"num":22,"den":1})"), "a contrast above 21",
                         "ValidationFailed");
    CheckFailureEnvelope(call(R"({"num":1,"den":0})"), "a zero denominator",
                         "ValidationFailed");
    CheckFailureEnvelope(call("4.5"), "a decimal contrast", "ValidationFailed");
}

void CheckPlanReadsRushCaches() {
    // PL2 -- a step of a plan reads a rush's caches through the session:
    // the plan's copy has none of its own. The same pauses, tightened
    // directly and through a plan, give the same bytes.
    Document pauses;
    pauses.sources = {
        {"01K30000000000000000000001", "rush.MP4", {25, 1}, {1000, 25}}};
    DocumentClip picture;
    picture.id = "01K30000000000000000000010";
    picture.source_id = "01K30000000000000000000001";
    picture.source_in = {0, 25};
    picture.duration = {75, 25};
    picture.timeline_in = {0, 25};
    pauses.sequence.tracks = {
        {"01K30000000000000000000013", "video", 0, {picture}}};
    SpeechOnsetReport envelope;
    envelope.media_id = "01K30000000000000000000001";
    envelope.windows_per_second = 50;
    envelope.decode_sample_rate = 16000;
    for (int index = 0; index < 150; ++index)
        envelope.levels.push_back(index >= 50 && index < 100 ? 20000 : 200000);
    envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
    envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);
    const std::string step =
        R"({"clip_id":"01K30000000000000000000010","min_gap_ms":300})";

    McpToolRegistry registry;
    InMemoryBackend direct(pauses);
    direct.SetSourceSpeechOnset(envelope);
    Check(CallWithJson(registry, direct, "tighten_pauses", step).ok,
          "the pauses tighten directly");

    InMemoryBackend planned(pauses);
    planned.SetSourceSpeechOnset(envelope);
    const McpToolCallOutcome prepared =
        CallWithJson(registry, planned, "prepare_edit_plan",
                     R"({"label":"Pauses","steps":[{"tool":"tighten_pauses",)"
                     R"("arguments":)" +
                         step + "}]}");
    const mcp_json::Value plan = ParsedJson(prepared.result_json);
    Check(prepared.ok && plan.Find("plan_id") != nullptr &&
              planned.Log().AppliedCount() == 0,
          "a plan step reads the rush's speech envelope and applies "
          "nothing: " +
              prepared.message);
    if (plan.Find("plan_id") == nullptr) return;
    mcp_json::Value apply = mcp_json::Value::MakeObject();
    apply.Set("plan_id", *plan.Find("plan_id"));
    // The clip a cut creates takes a fresh ULID on each side: the cuts are
    // compared, not the ids.
    const auto cuts = [](const Document& document) {
        const auto time = [](const RationalTime& value) {
            return std::to_string(value.value) + '/' +
                   std::to_string(value.rate);
        };
        std::string spans;
        for (const DocumentTrack& track : document.sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                spans += time(clip.source_in) + '+' + time(clip.duration) +
                         '@' + time(clip.timeline_in) + ' ';
        return spans;
    };
    const McpToolCallOutcome appliedPlan =
        registry.Call(planned, "apply_edit_plan", apply);
    Check(appliedPlan.ok &&
              planned.CurrentDocument().sequence.tracks[0].clips.size() == 2 &&
              cuts(planned.CurrentDocument()) == cuts(direct.CurrentDocument()),
          "the plan applies the very cut the direct call made: " +
              appliedPlan.message + ' ' + cuts(planned.CurrentDocument()) +
              "/ " + cuts(direct.CurrentDocument()));
}

int main() {
    // Built once: Document's default sequence ID is itself a fresh
    // GenerateUlid() on every construction, so two independent Fixture()
    // calls would legitimately disagree on sequence.id alone. Every
    // "expected" comparison below copies this same fixture rather than
    // calling Fixture() again.
    const Document fixture = Fixture();

    // DB1 -- capture all timelines without changing the editor, and query
    // the archive even when this server has no project open.
    {
        class ArchiveBackend : public InMemoryBackend {
        public:
            explicit ArchiveBackend(const Document& document)
                : InMemoryBackend(document),
                  project(Project::FromDocument(document)) {}
            Project project;
            bool open = true;
            bool HasOpenProject() const override { return open; }
            bool SnapshotProject(Project& result, std::string&) override {
                result = project;
                return true;
            }
            bool SnapshotDocument(Document&, std::string& message) override {
                message = "archive tools must not require a timeline snapshot";
                return false;
            }
        } backend(fixture);
        const auto directory = std::filesystem::temp_directory_path() /
                               ("cutmachine-mcp-archive-" + GenerateUlid());
        std::filesystem::create_directory(directory);
        const std::string database = (directory / "projects.sqlite").string();
        McpToolRegistry registry;
        auto args = mcp_json::Value::MakeObject();
        args.Set("database", mcp_json::Value::MakeString(database));
        args.Set("snapshot_id", mcp_json::Value::MakeString(GenerateUlid()));
        args.Set("step", mcp_json::Value::MakeString("selection"));
        const std::string before = backend.project.SaveToString();
        const auto captured = registry.Call(backend, "archive_project", args);
        Check(captured.ok && backend.project.SaveToString() == before,
              "archive_project captures the whole project without a timeline "
              "read or edit: " +
                  captured.message);
        const auto retried = registry.Call(backend, "archive_project", args);
        Check(retried.ok && retried.result_json.find("\"replayed\":true") !=
                                std::string::npos,
              "archive_project retries use the persisted snapshot ID");
        backend.open = false;
        const auto refused = registry.Call(backend, "archive_project", args);
        Check(!refused.ok && refused.error_name == "NoProjectOpen",
              "capturing still requires an open project");
        args = mcp_json::Value::MakeObject();
        args.Set("database", mcp_json::Value::MakeString(database));
        args.Set("sql", mcp_json::Value::MakeString(
                            "SELECT count(*) FROM project_snapshots"));
        const auto queried =
            registry.Call(backend, "query_project_archive", args);
        Check(queried.ok && queried.result_json.find("\"rows\":[[1]]") !=
                                std::string::npos,
              "query_project_archive works without an open project: " +
                  queried.message);
        args.Set("max_rows", mcp_json::Value::MakeDouble(1.5));
        const auto badLimit =
            registry.Call(backend, "query_project_archive", args);
        Check(!badLimit.ok && badLimit.error_name == "ValidationFailed",
              "query row limit is an exact integer");
        args.Set("max_rows", mcp_json::Value::MakeInt(10));
        args.Set("sql",
                 mcp_json::Value::MakeString("DELETE FROM project_snapshots"));
        Check(!registry.Call(backend, "query_project_archive", args).ok,
              "MCP SQL cannot delete a captured project");
        // DEC44 -- the archive's copy, without a project, into the folder
        // of the machine's setting; refused by name without one.
        const auto backups = directory / "Copies";
        std::filesystem::create_directory(backups);
        setenv("CUTMACHINE_PROJECT_ARCHIVE", database.c_str(), 1);
        const auto unset = registry.Call(backend, "backup_project_archive",
                                         mcp_json::Value::MakeObject());
        Check(!unset.ok && unset.error_name == "NotConfigured",
              "no folder, no copy: " + unset.error_name);
        setenv("CUTMACHINE_ARCHIVE_BACKUP_DIR", backups.c_str(), 1);
        const auto copied = registry.Call(backend, "backup_project_archive",
                                          mcp_json::Value::MakeObject());
        Check(copied.ok &&
                  copied.result_json.find("\"performed\":true") !=
                      std::string::npos &&
                  std::filesystem::exists(
                      backups /
                      project_archive::BackupFileName(std::time(nullptr))),
              "backup_project_archive copies the archive without a "
              "project: " +
                  copied.message);
        auto extra = mcp_json::Value::MakeObject();
        extra.Set("directory", mcp_json::Value::MakeString("/tmp"));
        Check(!registry.Call(backend, "backup_project_archive", extra).ok,
              "the folder is the machine's setting, never an argument");
        setenv("CUTMACHINE_ARCHIVE_BACKUP_DIR", "off", 1);
        setenv("CUTMACHINE_PROJECT_ARCHIVE", "off", 1);
        std::filesystem::remove_all(directory);
    }

    // add_track: video, audio and caption tracks share one numbering. Index 1
    // for a second video track, beside the audio track at 1, used to be
    // refused as "not unique"; the refusal now names the holder and the
    // first free index, and an omitted index is one above every track.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        auto args = mcp_json::Value::MakeObject();
        args.Set("kind", mcp_json::Value::MakeString("video"));
        args.Set("index", mcp_json::Value::MakeInt(1));
        const auto taken = registry.Call(backend, "add_track", args);
        Check(!taken.ok &&
                  taken.message.find(
                      "taken by audio track '01K30000000000000000000005'") !=
                      std::string::npos &&
                  taken.message.find("first free index from there is 2") !=
                      std::string::npos,
              "a taken track index names its holder and the next free one: " +
                  taken.message);
        auto omitted = mcp_json::Value::MakeObject();
        omitted.Set("kind", mcp_json::Value::MakeString("video"));
        const auto added = registry.Call(backend, "add_track", omitted);
        const auto& tracks = backend.CurrentDocument().sequence.tracks;
        Check(added.ok && tracks.size() == 3 && tracks.back().index == 2 &&
                  tracks.back().kind == "video",
              "an omitted track index is one above every track: " +
                  added.message);
    }

    // ADOBE-CLI-2026-09 -- range commands share the monitor's engine edits,
    // including targeted clear, sync-locked ripple and linked Source audio.
    {
        Document rangeFixture = fixture;
        rangeFixture.sequence.tracks.clear();
        for (int index = 0; index < 4; ++index) {
            DocumentTrack track{
                GenerateUlid(), index == 0 ? "video" : "audio", index, {}};
            track.locked = index == 3;
            track.sync_lock = index != 2;
            track.clips = {{GenerateUlid(),
                            fixture.sources[0].id,
                            {0, 25},
                            {100, 25},
                            {0, 25}},
                           {GenerateUlid(),
                            fixture.sources[0].id,
                            {150, 25},
                            {50, 25},
                            {150, 25}}};
            rangeFixture.sequence.tracks.push_back(std::move(track));
        }
        const auto time = [](int64_t value, int32_t rate = 25) {
            auto result = mcp_json::Value::MakeObject();
            result.Set("value", mcp_json::Value::MakeInt(value));
            result.Set("rate", mcp_json::Value::MakeInt(rate));
            return result;
        };
        const auto arrayId = [](const Ulid& id) {
            auto result = mcp_json::Value::MakeArray();
            result.Push(mcp_json::Value::MakeString(id));
            return result;
        };
        const Ulid target = rangeFixture.sequence.tracks[0].id;
        const Ulid audio = rangeFixture.sequence.tracks[1].id;
        const std::string original = rangeFixture.SaveToString();
        const auto sameTrack = [&](const DocumentTrack& left,
                                   const DocumentTrack& right) {
            Document a = rangeFixture;
            Document b = rangeFixture;
            a.sequence.tracks = {left};
            b.sequence.tracks = {right};
            return a.SaveToString() == b.SaveToString();
        };
        McpToolRegistry registry;
        auto sourceArgs = mcp_json::Value::MakeObject();
        sourceArgs.Set("source_id",
                       mcp_json::Value::MakeString(fixture.sources[0].id));
        sourceArgs.Set("track_id", mcp_json::Value::MakeString(target));
        sourceArgs.Set("source_in", time(300));
        sourceArgs.Set("timeline_in", time(50));
        sourceArgs.Set("timeline_out", time(75));
        sourceArgs.Set("audio_track_ids", arrayId(audio));
        for (const std::string mode : {"Insert", "Overwrite"}) {
            InMemoryBackend backend(rangeFixture);
            sourceArgs.Set("mode", mcp_json::Value::MakeString(mode));
            sourceArgs.Set("preview", mcp_json::Value::MakeBool(true));
            const auto preview =
                registry.Call(backend, "edit_source_range", sourceArgs);
            Check(preview.ok &&
                      backend.CurrentDocument().SaveToString() == original &&
                      backend.Log().AppliedCount() == 0,
                  mode + " preview validates without a journal entry: " +
                      preview.message);
            sourceArgs.Set("preview", mcp_json::Value::MakeBool(false));
            const auto applied =
                registry.Call(backend, "edit_source_range", sourceArgs);
            Check(applied.ok && backend.Log().AppliedCount() == 1,
                  mode + " Source edit is one operation: " + applied.message);
            if (!applied.ok) {
                continue;
            }
            const auto& tracks = backend.CurrentDocument().sequence.tracks;
            const auto& clips = tracks[0].clips;
            Check(clips.size() == 4 &&
                      clips[1].source_in == RationalTime{300, 25} &&
                      clips[1].duration == RationalTime{25, 25} &&
                      clips[1].timeline_in == RationalTime{50, 25} &&
                      !clips[1].include_audio &&
                      !clips[1].link_group_id.empty() &&
                      tracks[1].clips[1].link_group_id ==
                          clips[1].link_group_id &&
                      tracks[1].clips[1].sync_anchor_clip_id == clips[1].id,
                  mode + " inserts an exact linked video/audio Source range");
            Check(clips[3].timeline_in ==
                          RationalTime{mode == "Insert" ? 175 : 150, 25} &&
                      sameTrack(tracks[2], rangeFixture.sequence.tracks[2]) &&
                      sameTrack(tracks[3], rangeFixture.sequence.tracks[3]),
                  mode + " obeys sync lock and leaves locked tracks unchanged");
            const std::string after = backend.CurrentDocument().SaveToString();
            Operation parsed;
            EditError error = EditError::None;
            std::string message;
            const std::string canonical =
                SerializeOperation(backend.Log().AppliedEntries().back().op);
            Check(DeserializeOperation(canonical, parsed, error, message) &&
                      SerializeOperation(parsed) == canonical,
                  mode + " serializes canonically with all reserved clip IDs");
            Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject())
                          .ok &&
                      backend.CurrentDocument().SaveToString() == original &&
                      registry
                          .Call(backend, "redo", mcp_json::Value::MakeObject())
                          .ok &&
                      backend.CurrentDocument().SaveToString() == after,
                  mode + " undo restores bytes and redo restores the same IDs");
        }
        auto deleteArgs = mcp_json::Value::MakeObject();
        deleteArgs.Set("in", time(25));
        deleteArgs.Set("out", time(50));
        deleteArgs.Set("track_ids", arrayId(target));
        for (const bool ripple : {false, true}) {
            InMemoryBackend backend(rangeFixture);
            deleteArgs.Set("ripple", mcp_json::Value::MakeBool(ripple));
            deleteArgs.Set("preview", mcp_json::Value::MakeBool(true));
            const auto preview =
                registry.Call(backend, "delete_timeline_range", deleteArgs);
            Check(preview.ok &&
                      backend.CurrentDocument().SaveToString() == original &&
                      backend.Log().AppliedCount() == 0,
                  "Record preview changes neither document nor journal");
            deleteArgs.Set("preview", mcp_json::Value::MakeBool(false));
            const auto applied =
                registry.Call(backend, "delete_timeline_range", deleteArgs);
            Check(applied.ok && backend.Log().AppliedCount() == 1,
                  "Record clear/extract applies as one exact operation: " +
                      applied.message);
            if (!applied.ok) {
                continue;
            }
            // PRD40 -- ROADMAP.md. The delta every edit returns; a preview,
            // which changes nothing, has none.
            {
                mcp_json::Value previewView;
                mcp_json::Value appliedView;
                std::string viewError;
                const mcp_json::Value* delta = nullptr;
                Check(mcp_json::Value::Parse(preview.result_json, previewView,
                                             viewError) &&
                          previewView.Find("delta") == nullptr &&
                          mcp_json::Value::Parse(applied.result_json,
                                                 appliedView, viewError) &&
                          (delta = appliedView.Find("delta")) != nullptr &&
                          delta->Find("clips") != nullptr,
                      "delete_timeline_range answers with its delta, its "
                      "preview without: " +
                          applied.result_json.substr(0, 400));
            }
            const auto& tracks = backend.CurrentDocument().sequence.tracks;
            Check(
                tracks[0].clips.size() == 3 &&
                    tracks[0].clips[0].duration == RationalTime{25, 25} &&
                    tracks[0].clips[1].source_in == RationalTime{50, 25} &&
                    tracks[0].clips[1].timeline_in ==
                        RationalTime{ripple ? 25 : 50, 25} &&
                    tracks[0].clips[2].timeline_in ==
                        RationalTime{ripple ? 125 : 150, 25} &&
                    tracks[1].clips.size() == (ripple ? 3 : 2) &&
                    sameTrack(tracks[2], rangeFixture.sequence.tracks[2]) &&
                    sameTrack(tracks[3], rangeFixture.sequence.tracks[3]),
                "clear leaves the range empty; extract closes sync-locked time "
                "only");
            const std::string after = backend.CurrentDocument().SaveToString();
            Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject())
                          .ok &&
                      backend.CurrentDocument().SaveToString() == original &&
                      registry
                          .Call(backend, "redo", mcp_json::Value::MakeObject())
                          .ok &&
                      backend.CurrentDocument().SaveToString() == after,
                  "Record undo/redo preserves canonical bytes and split "
                  "identities");
        }
        const auto refused = [&](const std::string& tool,
                                 const mcp_json::Value& args) {
            InMemoryBackend backend(rangeFixture);
            const auto outcome = registry.Call(backend, tool, args);
            CheckFailureEnvelope(outcome, tool + " invalid range",
                                 "ValidationFailed");
            Check(backend.CurrentDocument().SaveToString() == original &&
                      backend.Log().AppliedCount() == 0,
                  tool + " refusal writes no partial operation");
        };
        auto bad = sourceArgs;
        bad.Set("source_out", time(400));
        refused("edit_source_range", bad);
        bad = sourceArgs;
        bad.Set("audio_track_ids", arrayId(target));
        refused("edit_source_range", bad);
        bad = sourceArgs;
        bad.Set("mode", mcp_json::Value::MakeString("Unknown"));
        refused("edit_source_range", bad);
        bad = sourceArgs;
        bad.Set("source_in", time(999));
        refused("edit_source_range", bad);
        bad = deleteArgs;
        bad.Set("in", time(-1));
        refused("delete_timeline_range", bad);
        bad = deleteArgs;
        bad.Set("out", time(25));
        refused("delete_timeline_range", bad);
        bad = deleteArgs;
        bad.Set("track_ids", mcp_json::Value::MakeArray());
        refused("delete_timeline_range", bad);
        bad = deleteArgs;
        bad.Set("track_ids", arrayId(fixture.sources[0].id));
        refused("delete_timeline_range", bad);
        bad = deleteArgs;
        bad.Set("track_ids", arrayId(rangeFixture.sequence.tracks[3].id));
        refused("delete_timeline_range", bad);
        bad = deleteArgs;
        auto duplicates = arrayId(target);
        duplicates.Push(mcp_json::Value::MakeString(target));
        bad.Set("track_ids", std::move(duplicates));
        refused("delete_timeline_range", bad);

        Document liveDocument = rangeFixture;
        EditLog liveLog;
        McpLiveBackend liveBackend(liveDocument, liveLog);
        sourceArgs.Set("preview", mcp_json::Value::MakeBool(false));
        Check(
            registry.Call(liveBackend, "edit_source_range", sourceArgs).ok &&
                liveLog.AppliedCount() == 1 &&
                registry
                    .Call(liveBackend, "undo", mcp_json::Value::MakeObject())
                    .ok &&
                liveDocument.SaveToString() == original,
            "Source range uses the live document and its shared undo journal");
        const McpToolCallOutcome liveDelete =
            registry.Call(liveBackend, "delete_timeline_range", deleteArgs);
        Check(liveDelete.ok && liveLog.AppliedCount() == 1 &&
                  registry
                      .Call(liveBackend, "undo", mcp_json::Value::MakeObject())
                      .ok &&
                  liveDocument.SaveToString() == original,
              "Record range also edits and undoes through the live backend");
        // PRD40 -- the live backend's own delta, carried through.
        Check(liveDelete.result_json.find("\"delta\":{") != std::string::npos,
              "delete_timeline_range carries the live backend's delta: " +
                  liveDelete.result_json.substr(0, 300));

        InMemoryBackend backend(rangeFixture);
        sourceArgs.Set("preview", mcp_json::Value::MakeBool(true));
        const auto preview =
            registry.Call(backend, "edit_source_range", sourceArgs);
        mcp_json::Value result;
        std::string message;
        Operation prepared;
        EditError error = EditError::None;
        Check(
            preview.ok &&
                mcp_json::Value::Parse(preview.result_json, result, message) &&
                (result.Find("operation") != nullptr) &&
                DeserializeOperation(result.Find("operation")->Dump(), prepared,
                                     error, message),
            "preview exposes a replayable canonical range operation");
        std::string applied;
        std::string errorName;
        Check(backend.ApplyOperation(
                  Operation{SetTrackLockOperation{
                      rangeFixture.sequence.tracks[2].id, true}},
                  applied, errorName, message),
              "simulate an intervening journaled mouse edit");
        const std::string concurrent = backend.CurrentDocument().SaveToString();
        Check(!backend.ApplyPreparedOperation(prepared, original, applied,
                                              errorName, message) &&
                  errorName == "StaleDocument" &&
                  backend.CurrentDocument().SaveToString() == concurrent &&
                  backend.Log().AppliedCount() == 1,
              "a prepared range refuses a stale snapshot and preserves the "
              "concurrent edit");
    }

    // B2 -- every registered tool crosses the same refusal boundary. A
    // non-object arguments value is invalid for all tools, including the
    // no-argument ones, so this walks the catalog without mutating the
    // fixture or depending on each tool's required fields.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const mcp_json::Value invalidArguments = mcp_json::Value::MakeArray();
        Check(!registry.Tools().empty(), "the MCP catalog is not empty");
        for (const McpTool& tool : registry.Tools()) {
            const McpToolCallOutcome refused =
                registry.Call(backend, tool.name, invalidArguments);
            CheckFailureEnvelope(refused, "catalog tool " + tool.name,
                                 "ValidationFailed");
        }
        const McpToolCallOutcome unknown =
            registry.Call(backend, "not_a_tool", invalidArguments);
        CheckFailureEnvelope(unknown, "unknown catalog tool", "UnknownTool");
    }

    // F1.3 -- wheels and curves. The description is what an agent reads to
    // grade: it names the new stages, their params and the bounds the
    // document enforces; a refused curve says why.
    {
        McpToolRegistry registry;
        const auto tool =
            std::find_if(registry.Tools().begin(), registry.Tools().end(),
                         [](const McpTool& candidate) {
                             return candidate.name == "set_clip_effects";
                         });
        const std::string description =
            tool == registry.Tools().end() ? "" : tool->description;
        for (const char* expected : {"color.wheels",
                                     "lift_master",
                                     "gamma_*",
                                     "gain_*",
                                     "offset_*",
                                     "offset_c + offset_master",
                                     "[-1, 1]",
                                     "color.hue (degrees",
                                     "color.contrast also takes pivot",
                                     "color.curves",
                                     "color.hue_curves",
                                     "hue_hue, hue_sat, hue_lum",
                                     "measure_color's hue angle / 360",
                                     "<channel>_x<i>",
                                     "<channel>_y<i>",
                                     "2 to 16",
                                     "x strictly increasing",
                                     "x and y in [0, 1]",
                                     "never overshoots",
                                     "ACEScct"}) {
            Check(description.find(expected) != std::string::npos,
                  std::string("set_clip_effects describes ") + expected);
        }
        InMemoryBackend backend(fixture);
        mcp_json::Value arguments;
        std::string parseError;
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","effects":[)"
                R"({"type":"color.curves","params":{)"
                R"("master_x0":{"num":1,"den":2},"master_y0":{"num":0,"den":1},)"
                R"("master_x1":{"num":1,"den":2},"master_y1":{"num":1,"den":1})"
                R"(}}]})",
                arguments, parseError),
            "curve arguments parse: " + parseError);
        const McpToolCallOutcome refused =
            registry.Call(backend, "set_clip_effects", arguments);
        CheckFailureEnvelope(refused, "a curve that does not advance in x",
                             "ValidationFailed");
        Check(refused.message.find("not increasing in x") != std::string::npos,
              "the refusal says why: " + refused.message);
        Check(
            backend.CurrentDocument().SaveToString() == fixture.SaveToString(),
            "a refused curve changes nothing");

        // GR7 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A stack longer than
        // a clip's grade renders is refused by what builds it, and named.
        std::string stages;
        for (int index = 0; index < 17; ++index) {
            stages += std::string(index == 0 ? "" : ",") +
                      R"({"type":"color.exposure","params":{"amount":)"
                      R"({"num":1,"den":100}}})";
        }
        mcp_json::Value tooMany;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003","effects":[)" +
                      stages + "]}",
                  tooMany, parseError),
              "seventeen stages parse: " + parseError);
        const McpToolCallOutcome overflowing =
            registry.Call(backend, "set_clip_effects", tooMany);
        CheckFailureEnvelope(overflowing, "a seventeenth stage",
                             "ValidationFailed");
        Check(overflowing.message.find("stage 17 (color.exposure)") !=
                      std::string::npos &&
                  backend.CurrentDocument().SaveToString() ==
                      fixture.SaveToString(),
              "the refusal names the stage the picture would stop before, "
              "and nothing changes: " +
                  overflowing.message);

        // GR6 -- a contrast about its pivot and a hue rotation go through,
        // and the document declares the version that carries them.
        mcp_json::Value pivoted;
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","effects":[)"
                R"({"type":"color.contrast","params":{)"
                R"("amount":{"num":1,"den":5},"pivot":{"num":3,"den":10}}},)"
                R"({"type":"color.hue","params":{"degrees":{"num":-25,"den":1}}})"
                R"(]})",
                pivoted, parseError),
            "pivot arguments parse: " + parseError);
        const McpToolCallOutcome graded =
            registry.Call(backend, "set_clip_effects", pivoted);
        Check(graded.ok && backend.CurrentDocument().SaveToString().find(
                               "\"version\": 17") != std::string::npos,
              "a pivot and a hue rotation are applied, at version 17: " +
                  graded.message);
        mcp_json::Value outside;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003","effects":[)"
                  R"({"type":"color.contrast","params":{)"
                  R"("pivot":{"num":3,"den":2}}}]})",
                  outside, parseError),
              "an outside pivot parses: " + parseError);
        const McpToolCallOutcome refusedPivot =
            registry.Call(backend, "set_clip_effects", outside);
        CheckFailureEnvelope(refusedPivot, "a pivot outside [0, 1]",
                             "ValidationFailed");
        Check(refusedPivot.message.find("outside [0, 1]") != std::string::npos,
              "the refusal says why: " + refusedPivot.message);
    }

    // F1.5 -- ROADMAP.md. Multicam tools: prefixes, created ids, timeline_id,
    // one undo step each, and the uniform refusal envelope.
    {
        McpToolRegistry registry;
        Document multicam = fixture;
        const Ulid wide = "01K30000000000000000000003";
        const Ulid close = "01K3M000000000000000000021";
        multicam.sequence.tracks.push_back({"01K3T000000000000000000020",
                                            "video",
                                            2,
                                            {{close,
                                              "01K30000000000000000000001",
                                              {300, 25},
                                              {10, 25},
                                              {5, 25}}}});
        InMemoryBackend backend(multicam);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& tool,
                              const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const auto parse = [](const McpToolCallOutcome& outcome) {
            mcp_json::Value value;
            std::string parseError;
            mcp_json::Value::Parse(outcome.result_json, value, parseError);
            return value;
        };
        const auto hidden = [](const mcp_json::Value& result) {
            std::vector<std::string> ids;
            if (const mcp_json::Value* list = result.Find("hidden_clip_ids")) {
                for (const mcp_json::Value& id : list->AsArray()) {
                    ids.push_back(id.AsString());
                }
            }
            return ids;
        };

        const McpToolCallOutcome added =
            call("add_multicam_group",
                 R"({"timeline_id":")" + backend.CurrentDocument().sequence.id +
                     R"(","name":"Interview","angles":[{"clip_id":")" + wide +
                     R"(","name":"Large"},{"clip_id":"01K3M"}],)"
                     R"("active_clip_id":"01K3M"})");
        const mcp_json::Value addResult = parse(added);
        const mcp_json::Value* groupField = addResult.Find("group_id");
        const mcp_json::Value* angleIds = addResult.Find("angle_ids");
        Check(added.ok && (groupField != nullptr) && (angleIds != nullptr) &&
                  angleIds->AsArray().size() == 2 &&
                  hidden(addResult) == std::vector<std::string>{wide},
              "add_multicam_group resolves prefixes and returns what it "
              "created: " +
                  added.message + added.result_json);
        const std::string groupId =
            (groupField != nullptr) ? groupField->AsString() : "";
        const MulticamGroup* stored =
            backend.CurrentDocument().FindMulticamGroup(groupId);
        Check((stored != nullptr) && stored->angles.size() == 2 &&
                  stored->angles[0].name == "Large" &&
                  stored->angles[1].name == "Angle 2" &&
                  stored->active_angle_id == stored->angles[1].id,
              "the group is stored with its names and its active angle");

        const McpToolCallOutcome listed = call("list_multicam_groups", "{}");
        const mcp_json::Value listResult = parse(listed);
        Check(listed.ok && (listResult.Find("groups") != nullptr) &&
                  listResult.Find("groups")->AsArray().size() == 1 &&
                  hidden(listResult) == std::vector<std::string>{wide},
              "list_multicam_groups reports the group and what it hides");

        const McpToolCallOutcome switched = call(
            "set_multicam_active_angle", R"({"angle_id":")" + wide + R"("})");
        Check(switched.ok &&
                  hidden(parse(switched)) == std::vector<std::string>{close},
              "set_multicam_active_angle finds the angle from its clip: " +
                  switched.message);

        CheckFailureEnvelope(
            call("set_multicam_active_angle", R"({"angle_id":""})"),
            "clearing without group_id", "InvalidOperation");
        CheckFailureEnvelope(
            call("set_multicam_active_angle",
                 R"({"angle_id":"01K30000000000000000000004"})"),
            "a clip that is no angle", "UnknownMulticamAngle");
        CheckFailureEnvelope(call("set_multicam_active_angle", "{}"),
                             "angle_id omitted", "ValidationFailed");
        CheckFailureEnvelope(
            call("add_multicam_group",
                 R"({"angles":[{"clip_id":")" + wide + R"("}]})"),
            "a group of one angle", "InvalidOperation");
        CheckFailureEnvelope(
            call("add_multicam_group",
                 R"({"angles":[{"clip_id":")" + wide +
                     R"("},{"clip_id":"01K30000000000000000000004"}]})"),
            "a clip already in a group", "InvalidOperation");
        CheckFailureEnvelope(
            call("add_multicam_group",
                 R"({"angles":[{"clip_id":")" + wide + R"(","camera":"A"}]})"),
            "an unknown angle key", "ValidationFailed");
        CheckFailureEnvelope(
            call("remove_multicam_group", R"({"group_id":")" + wide + R"("})"),
            "a clip named as a group", "UnknownMulticamGroup");

        const McpToolCallOutcome cleared =
            call("set_multicam_active_angle",
                 R"({"group_id":")" + groupId + R"(","angle_id":""})");
        const mcp_json::Value clearResult = parse(cleared);
        Check(cleared.ok && hidden(clearResult).empty() &&
                  (clearResult.Find("active_angle_id") != nullptr) &&
                  clearResult.Find("active_angle_id")->AsString().empty(),
              "an empty angle_id with group_id clears the choice: " +
                  cleared.message);
        const McpToolCallOutcome removed = call(
            "remove_multicam_group", R"({"group_id":")" + groupId + R"("})");
        Check(removed.ok && (parse(removed).Find("group") != nullptr) &&
                  parse(removed).Find("group")->IsNull(),
              "remove_multicam_group dissolves the group: " + removed.message);
        Check(backend.CurrentDocument().SaveToString() == before,
              "dissolving leaves the clips exactly as they were");
        EditError undoError = EditError::None;
        std::string undoMessage;
        int undone = 0;
        while (
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage)) {
            ++undone;
        }
        Check(undone == 4 && backend.CurrentDocument().SaveToString() == before,
              "four multicam calls are four undo steps, byte-exact");
    }

    // ITW4 -- a caption style is created addressable and restyled in place.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(R"({"font_size":58,"outline_width":4,)"
                               R"("vertical_position":{"num":3,"den":4}})",
                               arguments, parseError);
        const McpToolCallOutcome added =
            registry.Call(backend, "add_caption_style", arguments);
        mcp_json::Value result;
        Check(
            added.ok &&
                mcp_json::Value::Parse(added.result_json, result, parseError) &&
                result.Find("style_id") != nullptr,
            "add_caption_style returns the style_id: " + added.message +
                added.result_json);
        const std::string styleId =
            result.Find("style_id") ? result.Find("style_id")->AsString() : "";
        const CaptionStyle* created =
            backend.CurrentDocument().FindCaptionStyle(styleId);
        Check(created && created->outline_width == 4 &&
                  created->vertical_position.den == 4,
              "the decoration is stored on the new style");
        mcp_json::Value::Parse(
            "{\"style_id\":\"" + styleId +
                "\",\"background_color\":\"#000000@0.5\","
                "\"vertical_position\":{\"num\":0,\"den\":7}}",
            arguments, parseError);
        const McpToolCallOutcome updated =
            registry.Call(backend, "update_caption_style", arguments);
        const CaptionStyle* restyled =
            backend.CurrentDocument().FindCaptionStyle(styleId);
        Check(updated.ok && restyled && restyled->outline_width == 4 &&
                  restyled->background_color == "#000000@0.5" &&
                  restyled->vertical_position.num == 0 &&
                  restyled->vertical_position.den == 1,
              "update_caption_style keeps unnamed settings and spells the "
              "preset position canonically: " +
                  updated.message);
        // PA4 -- docs/roadmap/PARITE-2026-11.md. The word animation is a
        // setting of the style; 'none' is how a tool takes it off.
        mcp_json::Value::Parse("{\"style_id\":\"" + styleId +
                                   "\",\"word_animation\":\"highlight\","
                                   "\"highlight_color\":\"#00ffcc\"}",
                               arguments, parseError);
        const McpToolCallOutcome animated =
            registry.Call(backend, "update_caption_style", arguments);
        const CaptionStyle* lit =
            backend.CurrentDocument().FindCaptionStyle(styleId);
        Check(animated.ok && lit && lit->word_animation == "highlight" &&
                  lit->highlight_color == "#00ffcc" && lit->outline_width == 4,
              "update_caption_style sets the word animation: " +
                  animated.message);
        mcp_json::Value::Parse(
            "{\"style_id\":\"" + styleId + "\",\"word_animation\":\"none\"}",
            arguments, parseError);
        const McpToolCallOutcome still =
            registry.Call(backend, "update_caption_style", arguments);
        const CaptionStyle* plain =
            backend.CurrentDocument().FindCaptionStyle(styleId);
        Check(still.ok && plain && plain->word_animation.empty() &&
                  plain->highlight_color == "#00ffcc",
              "'none' takes the animation off and keeps the colour: " +
                  still.message);
        mcp_json::Value::Parse(
            "{\"style_id\":\"" + styleId + "\",\"word_animation\":\"bounce\"}",
            arguments, parseError);
        const McpToolCallOutcome refused =
            registry.Call(backend, "update_caption_style", arguments);
        Check(!refused.ok,
              "an animation the renderer does not draw is refused");
    }

    // SY2, the window -- sync_audio on a recorder's sound already on the
    // timeline: slid into sync and linked, one undo, the same edit the
    // window's Auto Align applies. The offset is given, so nothing decodes.
    {
        Document document;
        document.sequence.frame_rate = {25, 1};
        document.sources = {
            {"01K30000000000000000000S01", "camera.mov", {25, 1}, {1500, 25}},
            {"01K30000000000000000000S02", "recorder.wav", {25, 1}, {1500, 25}},
        };
        DocumentClip picture{"01K30000000000000000000S03",
                             "01K30000000000000000000S01",
                             {50, 25},
                             {100, 25},
                             {200, 25}};
        DocumentClip camera = picture;
        camera.id = "01K30000000000000000000S04";
        picture.link_group_id = "01K30000000000000000000S05";
        picture.sync_anchor_clip_id = picture.id;
        camera.link_group_id = picture.link_group_id;
        camera.sync_anchor_clip_id = picture.id;
        DocumentClip dropped{"01K30000000000000000000S06",
                             "01K30000000000000000000S02",
                             {0, 1},
                             {1500, 25},
                             {0, 1}};
        DocumentTrack video;
        video.id = "01K30000000000000000000S07";
        video.kind = "video";
        video.clips = {picture};
        DocumentTrack cameraTrack;
        cameraTrack.id = "01K30000000000000000000S08";
        cameraTrack.kind = "audio";
        cameraTrack.index = 1;
        cameraTrack.clips = {camera};
        DocumentTrack recorderTrack;
        recorderTrack.id = "01K30000000000000000000S09";
        recorderTrack.kind = "audio";
        recorderTrack.index = 2;
        recorderTrack.clips = {dropped};
        document.sequence.tracks = {video, cameraTrack, recorderTrack};
        const McpToolRegistry registry;
        InMemoryBackend backend(document);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                  "sync_audio arguments parse: " + parseFailure);
            return registry.Call(backend, "sync_audio", arguments);
        };
        const auto both =
            call(R"({"clip_id":"01K30000000000000000000S03",)"
                 R"("sound_clip_id":"01K30000000000000000000S06",)"
                 R"("media_id":"01K30000000000000000000S02",)"
                 R"("offset":{"value":-123456,"rate":48000}})");
        CheckFailureEnvelope(both, "sync_audio with a sound clip and a media");
        const auto own = call(R"({"clip_id":"01K30000000000000000000S03",)"
                              R"("sound_clip_id":"01K30000000000000000000S04",)"
                              R"("offset":{"value":0,"rate":48000}})");
        CheckFailureEnvelope(own, "sync_audio on the picture's own sound",
                             "InvalidOperation");
        const auto aligned =
            call(R"({"clip_id":"01K30000000000000000000S03",)"
                 R"("sound_clip_id":"01K30000000000000000000S06",)"
                 R"("offset":{"value":-123456,"rate":48000}})");
        const DocumentClip* heard =
            backend.CurrentDocument().FindClip("01K30000000000000000000S06");
        Check(aligned.ok && heard != nullptr &&
                  heard->timeline_in == RationalTime{200, 25} &&
                  heard->duration == RationalTime{100, 25} &&
                  heard->source_in ==
                      RationalTime{50, 25}.add(RationalTime{123456, 48000}) &&
                  heard->link_group_id == "01K30000000000000000000S05" &&
                  aligned.result_json.find("\"placed\"") != std::string::npos,
              "sync_audio slides a sound clip into sync under the picture "
              "and links it: " +
                  aligned.message + aligned.result_json);
        const auto again =
            call(R"({"clip_id":"01K30000000000000000000S03",)"
                 R"("sound_clip_id":"01K30000000000000000000S06",)"
                 R"("offset":{"value":-123456,"rate":48000}})");
        Check(again.ok && again.result_json.find("\"unchanged\":true") !=
                              std::string::npos,
              "aligned again, it says nothing was left to do: " +
                  again.result_json);
        std::string undoResult;
        std::string undoError;
        std::string undoMessage;
        Check(
            backend.Undo(undoResult, undoError, undoMessage) &&
                backend.CurrentDocument().SaveToString() == before,
            "one undo gives the sound back where it was dropped: " + undoError);
    }

    // SN4 -- docs/roadmap/SON-2026-10.md. A stem cutmachine-sound rendered
    // from 01:00:00:00, laid at its BWF timecode on a new audio track, in
    // one undo; the media is named, never a time.
    {
        const std::filesystem::path wav =
            std::filesystem::temp_directory_path() /
            ("cutmachine-stem-" + GenerateUlid() + ".wav");
        {
            // 24-bit stereo, bext then fmt then data, as wav.mjs writes it:
            // 4 s at 48 kHz from sample 172800000 (01:00:00:00 at 25).
            std::string bytes;
            const auto put32 = [&](uint32_t value) {
                for (int shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<char>((value >> shift) & 0xff));
            };
            const auto put16 = [&](uint16_t value) {
                bytes.push_back(static_cast<char>(value & 0xff));
                bytes.push_back(static_cast<char>(value >> 8));
            };
            const uint32_t samples = 192000;
            bytes += "RIFF";
            put32(4 + 8 + 602 + 8 + 16 + 8 + samples * 6);
            bytes += "WAVEbext";
            put32(602);
            std::string bext(602, '\0');
            const uint32_t reference = 172800000;
            for (int index = 0; index < 4; ++index)
                bext[338 + index] =
                    static_cast<char>((reference >> (8 * index)) & 0xff);
            bytes += bext + "fmt ";
            put32(16);
            put16(1);
            put16(2);
            put32(48000);
            put32(48000 * 6);
            put16(6);
            put16(24);
            bytes += "data";
            put32(samples * 6);
            bytes += std::string(samples * 6, '\0');
            std::ofstream(wav, std::ios::binary) << bytes;
        }
        Document document;
        document.sequence.frame_rate = {25, 1};
        const Ulid stemId = "01K30000000000000000000T01";
        document.sources = {
            {stemId, wav.string(), {48000, 1}, {192000, 48000}}};
        LibraryMedia stem;
        stem.id = stemId;
        stem.path = wav.string();
        stem.filename = wav.filename().string();
        stem.codec = "pcm_s24le";
        stem.has_video = false;
        stem.orientation = "audio";
        stem.has_audio = true;
        stem.audio_rate = 48000;
        stem.audio_channels = 2;
        stem.rate = {48000, 1};
        stem.duration = {192000, 48000};
        document.library = {stem};
        DocumentTrack video;
        video.id = "01K30000000000000000000T02";
        video.kind = "video";
        document.sequence.tracks = {video};
        const McpToolRegistry registry;
        InMemoryBackend backend(document);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                  "place_sound_stem arguments parse: " + parseFailure);
            return registry.Call(backend, "place_sound_stem", arguments);
        };
        CheckFailureEnvelope(call(R"({"media_id":"01K30000000000000000000T01",)"
                                  R"("start_timecode":"02:00:00:00"})"),
                             "a stem before the timeline's start",
                             "InvalidTimelineIn");
        CheckFailureEnvelope(
            call(R"({"media_id":"01K30000000000000000000T01",)"
                 R"("track_id":"01K30000000000000000000T02"})"),
            "a stem on a video track", "InvalidOperation");
        CheckFailureEnvelope(
            call(R"({"media_id":"01K30000000000000000000T09"})"),
            "an unknown stem");
        const auto laid = call(R"({"media_id":"01K30000000000000000000T01"})");
        const Document& after = backend.CurrentDocument();
        const DocumentTrack* added = after.sequence.tracks.size() == 2
                                         ? &after.sequence.tracks[1]
                                         : nullptr;
        Check(laid.ok && added != nullptr && added->kind == "audio" &&
                  added->clips.size() == 1 &&
                  added->clips[0].timeline_in == RationalTime{0, 25} &&
                  added->clips[0].duration == RationalTime{100, 25} &&
                  laid.result_json.find("\"first_frame\":0") !=
                      std::string::npos &&
                  laid.result_json.find("\"track_added\":true") !=
                      std::string::npos,
              "place_sound_stem lays the stem from the first frame on a new "
              "audio track: " +
                  laid.message + laid.result_json);
        std::string undoResult;
        std::string undoError;
        std::string undoMessage;
        Check(backend.Undo(undoResult, undoError, undoMessage) &&
                  backend.CurrentDocument().SaveToString() == before,
              "one undo takes the track and the stem away: " + undoError);
        std::filesystem::remove(wav);
    }

    // SN7 -- docs/roadmap/SON-2026-10.md. render_sound's refusals, before any
    // render: arguments, a stem with nowhere to go, a host that is not there.
    // The render itself is proved against the Node CLI by
    // tests/sound_project_tests.mjs, which has the host and FFmpeg.
    {
        const McpToolRegistry registry;
        InMemoryBackend backend(Document{});
        const auto call = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                  "render_sound arguments parse: " + parseFailure);
            return registry.Call(backend, "render_sound", arguments);
        };
        CheckFailureEnvelope(call(R"({})"), "render_sound without a ditty",
                             "ValidationFailed");
        CheckFailureEnvelope(call(R"({"ditty":"a.js","sample_rate":12})"),
                             "render_sound at 12 Hz", "ValidationFailed");
        CheckFailureEnvelope(call(R"({"ditty":"a.js","timeout":0})"),
                             "render_sound without time", "ValidationFailed");
        CheckFailureEnvelope(call(R"({"ditty":"a.js","settings":[1]})"),
                             "render_sound with settings that are a list",
                             "ValidationFailed");
        CheckFailureEnvelope(call(R"({"ditty":"a.js","tail":"long"})"),
                             "render_sound with a tail in words",
                             "ValidationFailed");
        const auto nowhere = call(R"({"ditty":"a.js"})");
        CheckFailureEnvelope(nowhere, "render_sound with no package and no out",
                             "ValidationFailed");
        Check(nowhere.message.find("needs `out` when no project package") !=
                  std::string::npos,
              "the refusal says where the stem would go: " + nowhere.message);
        setenv("CUTMACHINE_SOUND_HOST", "/nonexistent/cutmachine-sound-host",
               1);
        CheckFailureEnvelope(
            call(R"({"ditty":"a.js","out":"/nonexistent/x.wav"})"),
            "render_sound with no host", "SoundHostMissing");
        unsetenv("CUTMACHINE_SOUND_HOST");
    }

    // ITW7 -- every tool that edits one timeline takes timeline_id, so an
    // agent never has to switch the active timeline first. set_clip_audio did
    // not. A tool missing from both lists below fails here, which is the
    // point: whoever adds it decides which one it belongs to.
    {
        const std::vector<std::string> kNotTimelineScoped = {
            // DB1 -- an archive captures a whole project; SQL reads the DB.
            "archive_project", "query_project_archive",
            "project_archive_status", "sync_project_archive",
            "import_project_archive",
            // DEC44 -- the archive's copy: no project at all.
            "backup_project_archive",
            // The project itself and its library (IA7).
            "create_project", "open_project", "ingest_media",
            // PRD17 -- import_resolve_timeline names the timeline it reads
            // into (timeline_id), like every edit: no longer listed here.
            // Project-level edits and reads, addressed by their own IDs.
            "add_timeline", "clone_timeline", "create_timeline_variants",
            "export_timelines", "list_timelines",
            // Source media analysis, addressed by media_id.
            "analyze_audio_beats", "analyze_shot_quality",
            "list_stable_windows", "analyze_hesitations",
            // PL3 -- docs/roadmap/PLANS-2026-10.md. The project's described
            // shots, by media_id: the library is shared by every timeline.
            "search_shots", "analyze_speech_onset", "list_audio_beats",
            "transcribe_media", "align_transcript",
            // AX2 -- NeMo on rushes, by media_id.
            "align_words",
            // AX5 -- the window's own timeline, whatever the session's.
            "get_editor_view",
            // VQA5 -- and the window itself.
            "capture_editor_window",
            // The derush sheet: rushes, addressed by media_id, before any
            // timeline holds them.
            "media_sheet",
            // AGENT-2026-09 A4 -- the words of one rush, by media_id.
            "list_source_words",
            // R7 -- the tempo grid of one source, addressed by media_id.
            // list_clip_tempo_beats places it on a timeline and does take
            // timeline_id, so it is not listed here.
            "list_tempo_grid",
            // The energy of that source on the same grid, by media_id.
            "list_music_structure",
            // External applications: the After Effects bridge addresses an
            // .aep and its compositions, never a CUTMACHINE timeline.
            "after_create_project", "after_describe_project",
            "after_save_project", "after_open_project_copy", "after_list_comps",
            "after_describe_comp", "after_list_footage", "after_render_frame",
            "after_set_text", "after_set_property", "after_set_layer_state",
            "after_replace_footage", "after_import_footage", "after_add_layer",
            "after_duplicate_layer",
            // PL1 -- the token names the timeline begin_intervention chose.
            "end_intervention",
            // IA5 -- background tasks, addressed by task_id.
            "get_task", "cancel_task", "list_tasks",
            // PL3 -- a plan names the timeline prepare_edit_plan chose. The
            // reads of a timeline (describe, get_timeline_transcript...) take
            // timeline_id since PL3, to read one without activating it.
            "apply_edit_plan", "discard_edit_plan",
            // OR2 -- serialized plans and run requests carry their target;
            // receipts are addressed by their full step ULID.
            "get_edit_plan", "commit_run_step", "get_run_step",
            // MOTION-2026-09 M3 -- reads of the project's compositions.
            "list_motion_compositions", "describe_motion_composition",
            "render_motion_frame",
            // M6 -- renders a composition to files, not a timeline.
            "render_motion_video",
            // MA2 -- a composition against the project After built from it.
            "verify_after_export",
            // M5 -- the templates the binary ships, before any project.
            "list_motion_templates",
            // AX6 -- the machine's font families, before any project.
            "list_fonts",
            // MB1 -- a composition, by its own id, to this machine's
            // library: no timeline is read or edited.
            "save_motion_template",
            // PRD18 -- the procedures the binary carries, no project at all.
            "get_procedure",
            // CF1 -- a local display preference, independent of timelines.
            "get_video_output", "set_video_output",
            // SYNCHRO-2026-10 SY1 -- two rushes, by media_id.
            "measure_audio_offset",
            // FC6 -- one rush, by media_id.
            "propose_ducking",
            // RM3 -- the forge reads corpus files, never a project.
            "train_caption_breaks",
            // GENERATIF-2026-10 GN2 -- a proposal names a rush by media_id
            // and spends nothing; generate_media, which edits, names its
            // timeline.
            "propose_generation",
            // PA3 -- one rush's voices, by media_id.
            "analyze_speakers", "list_speakers"};
        McpToolRegistry registry;
        for (const McpTool& tool : registry.Tools()) {
            mcp_json::Value schema;
            std::string schemaError;
            const mcp_json::Value* properties =
                mcp_json::Value::Parse(tool.input_schema_json, schema,
                                       schemaError)
                    ? schema.Find("properties")
                    : nullptr;
            const bool scoped = properties != nullptr &&
                                properties->Find("timeline_id") != nullptr;
            const bool exempt =
                std::find(kNotTimelineScoped.begin(), kNotTimelineScoped.end(),
                          tool.name) != kNotTimelineScoped.end();
            Check(scoped != exempt,
                  "tool " + tool.name +
                      (scoped ? " takes timeline_id but is listed as not "
                                "timeline-scoped"
                              : " edits without timeline_id, or is missing "
                                "from the not-timeline-scoped list"));
        }
    }

    // D6 -- docs/roadmap/DIALOGUE-2026-10.md. A marker with a duration is a
    // span: add_marker takes one, describe says it, and update_marker keeps
    // it when it is left out.
    {
        const McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const char* tool, const std::string& json) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                  std::string(tool) + " arguments parse: " + parseFailure);
            return registry.Call(backend, tool, arguments);
        };
        const auto point =
            call("add_marker",
                 R"({"name":"Un instant","time":{"value":12,"rate":25}})");
        const auto span =
            call("add_marker",
                 R"({"name":"Un passage","time":{"value":50,"rate":25},)"
                 R"("duration":{"value":75,"rate":25}})");
        Check(point.ok && span.ok, "add_marker takes a point and a span: " +
                                       point.message + span.message);
        const auto& markers = backend.CurrentDocument().sequence.markers;
        Check(markers.size() >= 2 &&
                  markers[markers.size() - 2].duration.value == 0 &&
                  markers.back().duration == RationalTime{75, 25},
              "the span has its duration, the point none");
        const Ulid spanId = markers.empty() ? Ulid{} : markers.back().id;

        // This backend describes the project it was built with, not the
        // document the calls edit: the snapshot is read from the document.
        const std::string described =
            DescribeDocument(backend.CurrentDocument());
        Check(described.find("\"name\":\"Un passage\",\"time\":{\"frames\":50,"
                             "\"seconds\":2.0},\"duration\":{\"frames\":75,"
                             "\"seconds\":3.0},") != std::string::npos &&
                  described.find("\"name\":\"Un instant\",\"time\":"
                                 "{\"frames\":12,\"seconds\":0.48},"
                                 "\"color\"") != std::string::npos,
              "describe gives a span's duration beside its time, and nothing "
              "for a point: " +
                  (described.find("\"markers\"") == std::string::npos
                       ? described
                       : described.substr(described.find("\"markers\""), 500)));

        const auto renamed =
            call("update_marker",
                 R"({"marker_id":")" + spanId +
                     R"(","name":"Le passage","time":{"value":50,"rate":25},)"
                     R"("color":"#4da3ff","category":"note"})");
        const DocumentMarker* after =
            backend.CurrentDocument().FindMarker(spanId);
        Check(renamed.ok && after && after->name == "Le passage" &&
                  after->duration == RationalTime{75, 25},
              "update_marker without a duration keeps the span: " +
                  renamed.message);
        const auto shortened =
            call("update_marker",
                 R"({"marker_id":")" + spanId +
                     R"(","name":"Le passage","time":{"value":50,"rate":25},)"
                     R"("duration":{"value":0,"rate":1},)"
                     R"("color":"#4da3ff","category":"note"})");
        after = backend.CurrentDocument().FindMarker(spanId);
        Check(shortened.ok && after && after->duration.value == 0,
              "update_marker with a zero duration makes it a point: " +
                  shortened.message);
        CheckFailureEnvelope(
            call("add_marker", R"({"name":"x","time":{"value":1,"rate":25},)"
                               R"("duration":{"value":-5,"rate":25}})"),
            "add_marker with a negative duration", "ValidationFailed");
    }

    // PL1 -- ROADMAP.md. An MCP client's intervention spans several tool
    // calls; begin_intervention and end_intervention make it one undo step.
    {
        const McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const std::string original = backend.CurrentDocument().SaveToString();
        const auto object =
            [](std::initializer_list<std::pair<const char*, mcp_json::Value>>
                   fields) {
                auto value = mcp_json::Value::MakeObject();
                for (const auto& [key, field] : fields) value.Set(key, field);
                return value;
            };
        const auto time = [&](int64_t value) {
            return object({{"value", mcp_json::Value::MakeInt(value)},
                           {"rate", mcp_json::Value::MakeInt(25)}});
        };
        const auto begun = registry.Call(backend, "begin_intervention",
                                         mcp_json::Value::MakeObject());
        mcp_json::Value begunResult;
        std::string parseError;
        const mcp_json::Value* token =
            begun.ok && mcp_json::Value::Parse(begun.result_json, begunResult,
                                               parseError)
                ? begunResult.Find("intervention")
                : nullptr;
        Check(token && token->IsString() &&
                  begunResult.Find("history_depth") != nullptr &&
                  begunResult.Find("history_depth")->Dump() == "0",
              "begin_intervention answers with a token: " + begun.result_json);
        const auto trim = registry.Call(
            backend, "trim_clip",
            object({{"clip_id",
                     mcp_json::Value::MakeString("01K30000000000000000000003")},
                    {"edge", mcp_json::Value::MakeString("Tail")},
                    {"delta", time(-2)}}));
        const auto audio = registry.Call(
            backend, "set_clip_audio",
            object({{"clip_id",
                     mcp_json::Value::MakeString("01K30000000000000000000006")},
                    {"gain_db", object({{"num", mcp_json::Value::MakeInt(-6)},
                                        {"den", mcp_json::Value::MakeInt(1)}})},
                    {"fade_in", time(0)},
                    {"fade_out", time(3)}}));
        const auto marker = registry.Call(
            backend, "add_marker",
            object({{"name", mcp_json::Value::MakeString("Question 2")},
                    {"time", time(12)}}));
        Check(trim.ok && audio.ok && marker.ok,
              "three edits apply one call at a time: " + trim.message +
                  audio.message + marker.message);
        const std::string edited = backend.CurrentDocument().SaveToString();
        Check(backend.Log().AppliedCount() == 3, "three steps before the end");

        const auto withoutLabel = registry.Call(
            backend, "end_intervention",
            object({{"intervention", token ? *token : mcp_json::Value()}}));
        CheckFailureEnvelope(withoutLabel, "end_intervention without a label",
                             "ValidationFailed");
        const auto badToken = registry.Call(
            backend, "end_intervention",
            object({{"intervention", mcp_json::Value::MakeString("A1")},
                    {"label", mcp_json::Value::MakeString("Resserrer")}}));
        CheckFailureEnvelope(badToken, "end_intervention with a stray token",
                             "ValidationFailed");
        Check(backend.Log().AppliedCount() == 3,
              "a refused end leaves the history as it was");

        const auto ended = registry.Call(
            backend, "end_intervention",
            object({{"intervention", token ? *token : mcp_json::Value()},
                    {"label", mcp_json::Value::MakeString("Resserrer")}}));
        mcp_json::Value endedResult;
        Check(ended.ok &&
                  mcp_json::Value::Parse(ended.result_json, endedResult,
                                         parseError) &&
                  endedResult.Find("grouped") != nullptr &&
                  endedResult.Find("grouped")->Dump() == "3" &&
                  backend.Log().AppliedCount() == 1 &&
                  backend.CurrentDocument().SaveToString() == edited,
              "end_intervention folds three steps into one: " +
                  ended.result_json);
        Check(
            registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok &&
                backend.CurrentDocument().SaveToString() == original,
            "one undo tool call reverts the whole intervention");
        Check(
            registry.Call(backend, "redo", mcp_json::Value::MakeObject()).ok &&
                backend.CurrentDocument().SaveToString() == edited,
            "one redo tool call replays it");
    }

    // PL2/PL3 -- ROADMAP.md. Edit plans through the catalog: prepared on a
    // copy, aliases resolved once, applied as previewed in one undo step,
    // refused when stale, discarded without a trace.
    {
        const McpToolRegistry registry;
        Document threeClips = fixture;
        const Ulid first = "01K30000000000000000000003";
        const Ulid second = "01K30000000000000000000004";
        const Ulid third = "01K3P000000000000000000007";
        threeClips.sequence.tracks[0].clips.push_back(
            {third,
             "01K30000000000000000000001",
             {300, 25},
             {10, 25},
             {40, 25}});
        for (DocumentClip& clip : threeClips.sequence.tracks[0].clips) {
            clip.link_group_id.clear();
        }
        InMemoryBackend backend(threeClips);
        const std::string original = backend.CurrentDocument().SaveToString();
        const auto call = [&](McpBackend& target, const std::string& tool,
                              const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(target, tool, arguments);
        };
        const auto parse = [](const std::string& json) {
            mcp_json::Value value;
            std::string parseError;
            mcp_json::Value::Parse(json, value, parseError);
            return value;
        };
        const auto text = [](const mcp_json::Value& object, const char* key) {
            const mcp_json::Value* field = object.Find(key);
            return field != nullptr && field->IsString() ? field->AsString()
                                                         : std::string();
        };
        const auto removed = [](const mcp_json::Value& answer) {
            std::vector<std::string> ids;
            const mcp_json::Value* delta = answer.Find("delta");
            const mcp_json::Value* list =
                delta != nullptr ? delta->Find("removed_clip_ids") : nullptr;
            if (list != nullptr) {
                for (const mcp_json::Value& id : list->AsArray()) {
                    ids.push_back(id.AsString());
                }
            }
            return ids;
        };

        // Removing A1 then A2 removes the two clips observed, never the old
        // A3 that a second observation would have called A2.
        const McpToolCallOutcome prepared =
            call(backend, "prepare_edit_plan",
                 R"({"label":"Couper les deux premiers plans","steps":[)"
                 R"({"tool":"remove_clip","arguments":{"clip_id":"A1"}},)"
                 R"({"tool":"remove_clip","arguments":{"clip_id":"A2"}}]})");
        const mcp_json::Value preview = parse(prepared.result_json);
        Check(prepared.ok && text(preview, "status") == "prepared" &&
                  preview.Find("operation_count") != nullptr &&
                  preview.Find("operation_count")->Dump() == "2",
              "prepare_edit_plan answers with a prepared plan: " +
                  prepared.result_json);
        Check(removed(preview) == std::vector<std::string>{first, second},
              "aliases resolve against one observation: A2 is the second "
              "clip seen, not the old A3: " +
                  prepared.result_json);
        Check(backend.CurrentDocument().SaveToString() == original &&
                  backend.Log().AppliedCount() == 0,
              "preparing changes no byte of the timeline and no journal entry");
        Check(text(preview, "base_doc_hash") ==
                  TimelineRevision(backend.CurrentDocument()),
              "the plan names the revision it was prepared on");
        const mcp_json::Value* reports = preview.Find("steps");
        Check(reports != nullptr && reports->AsArray().size() == 2 &&
                  text(reports->AsArray()[1], "tool") == "remove_clip" &&
                  reports->AsArray()[1].Find("delta") == nullptr,
              "each step reports what its tool answered, minus the delta "
              "the plan states once");

        const std::string planId = text(preview, "plan_id");
        const McpToolCallOutcome applied =
            call(backend, "apply_edit_plan",
                 R"({"plan_id":")" + planId + R"(","verbose":true})");
        const mcp_json::Value appliedAnswer = parse(applied.result_json);
        Check(applied.ok && text(appliedAnswer, "status") == "applied" &&
                  text(appliedAnswer, "doc_hash") ==
                      text(preview, "result_doc_hash") &&
                  removed(appliedAnswer) == removed(preview),
              "apply_edit_plan lands where the preview said: " +
                  applied.result_json);
        const DocumentTrack& video =
            backend.CurrentDocument().sequence.tracks[0];
        Check(video.clips.size() == 1 && video.clips[0].id == third &&
                  backend.Log().AppliedCount() == 1,
              "the plan applied as one journal entry, the third clip kept");
        CheckFailureEnvelope(call(backend, "apply_edit_plan",
                                  R"({"plan_id":")" + planId + R"("})"),
                             "an applied plan applied again", "UnknownPlan");
        Check(
            registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok &&
                backend.CurrentDocument().SaveToString() == original,
            "one undo reverts the whole plan");

        // Identities created in the preview are the ones applied.
        const McpToolCallOutcome split =
            call(backend, "prepare_edit_plan",
                 R"({"label":"Couper A1","steps":[{"tool":"split_clip",)"
                 R"("arguments":{"clip_id":"A1","timeline_position":)"
                 R"({"value":10,"rate":25}}}]})");
        const mcp_json::Value splitPreview = parse(split.result_json);
        std::string createdInPreview;
        if (const mcp_json::Value* delta = splitPreview.Find("delta")) {
            if (const mcp_json::Value* clips = delta->Find("clips")) {
                for (const mcp_json::Value& clip : clips->AsArray()) {
                    const mcp_json::Value* created = clip.Find("created");
                    if (created != nullptr && created->AsBool()) {
                        createdInPreview = text(clip, "clip_id");
                    }
                }
            }
        }
        Check(
            split.ok && !createdInPreview.empty() &&
                backend.CurrentDocument().FindClip(createdInPreview) == nullptr,
            "the preview names the clip the split creates, not yet in the "
            "timeline: " +
                split.result_json);
        const McpToolCallOutcome splitApplied =
            call(backend, "apply_edit_plan",
                 R"({"plan_id":")" + text(splitPreview, "plan_id") + R"("})");
        Check(splitApplied.ok && backend.CurrentDocument().FindClip(
                                     createdInPreview) != nullptr,
              "the applied split creates exactly the previewed identity");
        // AX-2026-09 -- by default the answer points at the preview instead
        // of repeating it: same revision reached, no second delta.
        const mcp_json::Value splitAnswer = parse(splitApplied.result_json);
        const mcp_json::Value* asPrepared =
            splitAnswer.Find("delta_as_prepared");
        Check(asPrepared != nullptr && asPrepared->AsBool() &&
                  splitAnswer.Find("delta") == nullptr &&
                  splitAnswer.Find("steps") == nullptr &&
                  text(splitAnswer, "doc_hash") ==
                      text(splitPreview, "result_doc_hash"),
              "apply_edit_plan answers short, delta as prepared: " +
                  splitApplied.result_json);
        registry.Call(backend, "undo", mcp_json::Value::MakeObject());

        // Stale: an edit between preparation and application.
        const mcp_json::Value stalePreview =
            parse(call(backend, "prepare_edit_plan",
                       R"({"label":"Resserrer","steps":[{"tool":"trim_clip",)"
                       R"("arguments":{"clip_id":"A3","edge":"Tail",)"
                       R"("delta":{"value":-2,"rate":25}}}]})")
                      .result_json);
        Check(call(backend, "trim_clip",
                   R"({"clip_id":")" + first +
                       R"(","edge":"Tail","delta":{"value":-1,"rate":25}})")
                  .ok,
              "the timeline moves on after the plan was prepared");
        const std::string moved = backend.CurrentDocument().SaveToString();
        const McpToolCallOutcome stale =
            call(backend, "apply_edit_plan",
                 R"({"plan_id":")" + text(stalePreview, "plan_id") + R"("})");
        CheckFailureEnvelope(stale, "a stale plan", kStaleRevisionError);
        const mcp_json::Value staleEnvelope = parse(stale.result_json);
        const mcp_json::Value* staleResult = staleEnvelope.Find("result");
        Check(staleResult != nullptr &&
                  text(*staleResult, "base_doc_hash") ==
                      text(stalePreview, "base_doc_hash") &&
                  text(*staleResult, "doc_hash") ==
                      TimelineRevision(backend.CurrentDocument()),
              "the refusal names both revisions: " + stale.result_json);
        Check(backend.CurrentDocument().SaveToString() == moved &&
                  backend.Log().AppliedCount() == 1,
              "a stale plan applies nothing");
        const McpToolCallOutcome discarded =
            call(backend, "discard_edit_plan",
                 R"({"plan_id":")" + text(stalePreview, "plan_id") + R"("})");
        Check(discarded.ok &&
                  text(parse(discarded.result_json), "status") == "discarded" &&
                  registry.EditPlans().PlanIds().empty(),
              "discard_edit_plan forgets the plan: " + discarded.result_json);
        CheckFailureEnvelope(
            call(backend, "discard_edit_plan",
                 R"({"plan_id":")" + text(stalePreview, "plan_id") + R"("})"),
            "a plan discarded twice", "UnknownPlan");
        registry.Call(backend, "undo", mcp_json::Value::MakeObject());
        Check(backend.CurrentDocument().SaveToString() == original,
              "back to the original timeline");

        // What a step cannot be, and a step that fails: nothing is kept.
        CheckFailureEnvelope(call(backend, "prepare_edit_plan",
                                  R"({"label":"x","steps":[{"tool":"undo"}]})"),
                             "undo as a step", "UnsupportedOperation");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[{"tool":"describe"}]})"),
            "a read as a step", "UnsupportedOperation");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[{"tool":"prepare_edit_plan"}]})"),
            "a plan inside a plan", "UnsupportedOperation");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[{"tool":"no_such_tool"}]})"),
            "an unknown step", "UnknownTool");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan", R"({"label":"x","steps":[]})"),
            "a plan without steps", "ValidationFailed");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[{"tool":"remove_clip",)"
                 R"("arguments":{"clip_id":"A1","timeline_id":")" +
                     std::string("01K3S999999999999999999999") + R"("}}]})"),
            "a step aimed at another timeline", "ValidationFailed");
        CheckFailureEnvelope(
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[{"tool":"remove_clip",)"
                 R"("arguments":{"clip_id":"A1","idempotency_key":"k"}}]})"),
            "a session key inside a step", "ValidationFailed");
        const McpToolCallOutcome failing =
            call(backend, "prepare_edit_plan",
                 R"({"label":"x","steps":[)"
                 R"({"tool":"remove_clip","arguments":{"clip_id":"A1"}},)"
                 R"({"tool":"remove_clip","arguments":{"clip_id":"A1"}}]})");
        CheckFailureEnvelope(failing, "a step that fails", "UnknownClip");
        const mcp_json::Value failingEnvelope = parse(failing.result_json);
        const mcp_json::Value* failingResult = failingEnvelope.Find("result");
        Check(failingResult != nullptr &&
                  failingResult->Find("failed_step") != nullptr &&
                  failingResult->Find("failed_step")->Dump() == "2" &&
                  failing.message.find("step 2 (remove_clip)") !=
                      std::string::npos,
              "the refusal names the failing step: " + failing.result_json);
        Check(registry.EditPlans().PlanIds().empty() &&
                  backend.CurrentDocument().SaveToString() == original,
              "a refused plan keeps nothing and changes nothing");

        // Strict mode wants the timeline named when a plan is prepared.
        InMemoryBackend strict(threeClips);
        strict.RequireExplicitTimeline();
        const std::string steps =
            R"("label":"x","steps":[{"tool":"remove_clip",)"
            R"("arguments":{"clip_id":"A1"}}])";
        CheckFailureEnvelope(
            call(strict, "prepare_edit_plan", "{" + steps + "}"),
            "a strict plan without timeline_id", "TimelineRequired");
        const McpToolCallOutcome named =
            call(strict, "prepare_edit_plan",
                 R"({"timeline_id":")" + threeClips.sequence.id + R"(",)" +
                     steps + "}");
        Check(named.ok, "a strict plan naming its timeline is prepared: " +
                            named.message);

        // PL1 -- a plan applied inside an intervention folds into it.
        const McpToolCallOutcome begun =
            call(backend, "begin_intervention", "{}");
        const std::string token =
            text(parse(begun.result_json), "intervention");
        const McpToolCallOutcome inner =
            call(backend, "prepare_edit_plan",
                 R"({"label":"Plan","steps":[{"tool":"remove_clip",)"
                 R"("arguments":{"clip_id":"A1"}}]})");
        Check(call(backend, "apply_edit_plan",
                   R"({"plan_id":")" +
                       text(parse(inner.result_json), "plan_id") + R"("})")
                      .ok &&
                  call(backend, "trim_clip",
                       R"({"clip_id":")" + third +
                           R"(","edge":"Tail","delta":{"value":-1,"rate":25}})")
                      .ok,
              "a plan and an edit inside one intervention");
        const McpToolCallOutcome ended =
            call(backend, "end_intervention",
                 R"({"intervention":")" + token + R"(","label":"Tour"})");
        Check(ended.ok && backend.Log().AppliedCount() == 1 &&
                  registry.Call(backend, "undo", mcp_json::Value::MakeObject())
                      .ok &&
                  backend.CurrentDocument().SaveToString() == original,
              "the intervention folds the plan with the edit: one undo "
              "reverts both: " +
                  ended.result_json);

        // At most eight plans kept, refused rather than evicted.
        const McpToolRegistry crowded;
        InMemoryBackend crowd(threeClips);
        for (size_t index = 0; index < EditPlanStore::kMaxPlans; ++index) {
            Check(crowded
                      .Call(crowd, "prepare_edit_plan",
                            parse(R"({"label":"Variante","steps":[)"
                                  R"({"tool":"remove_clip",)"
                                  R"("arguments":{"clip_id":"A1"}}]})"))
                      .ok,
                  "plans on the same timeline coexist");
        }
        CheckFailureEnvelope(
            crowded.Call(crowd, "prepare_edit_plan",
                         parse(R"({"label":"Une de trop","steps":[)"
                               R"({"tool":"remove_clip",)"
                               R"("arguments":{"clip_id":"A1"}}]})")),
            "a ninth plan", kEditPlanLimitError);
    }

    // PL3 -- a read names its timeline without activating it. A backend
    // holding one document reads only that one; the app does not switch
    // timelines to read another.
    {
        const McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        mcp_json::Value own = mcp_json::Value::MakeObject();
        own.Set("timeline_id",
                mcp_json::Value::MakeString(fixture.sequence.id));
        for (const char* tool : {"describe", "timeline_stats",
                                 "list_multicam_groups", "list_shot_quality"}) {
            const McpToolCallOutcome read = registry.Call(backend, tool, own);
            Check(read.ok, std::string(tool) +
                               " reads the timeline it names: " + read.message);
        }
        mcp_json::Value other = mcp_json::Value::MakeObject();
        other.Set("timeline_id",
                  mcp_json::Value::MakeString("01K3S999999999999999999999"));
        CheckFailureEnvelope(registry.Call(backend, "describe", other),
                             "a read of a timeline this backend does not hold",
                             "UnknownSequence");
        // AGENT-2026-09 A25 -- a describe cut to the sections and track
        // kinds asked for.
        {
            const auto describe = [&](const std::string& json) {
                mcp_json::Value arguments;
                std::string parseFailure;
                Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                      "describe arguments parse: " + parseFailure);
                return registry.Call(backend, "describe", arguments);
            };
            mcp_json::Value view;
            std::string parseError;
            const McpToolCallOutcome sequenceOnly =
                describe(R"({"only":["sequence"]})");
            Check(sequenceOnly.ok &&
                      mcp_json::Value::Parse(sequenceOnly.result_json, view,
                                             parseError) &&
                      view.AsObject().size() == 1 &&
                      view.Find("sequence") != nullptr,
                  "only keeps the sections named: " +
                      sequenceOnly.result_json.substr(0, 200));
            const McpToolCallOutcome audioOnly =
                describe(R"({"only":["timeline"],"kinds":["audio"]})");
            bool allAudio = false;
            if (audioOnly.ok &&
                mcp_json::Value::Parse(audioOnly.result_json, view,
                                       parseError) &&
                view.Find("timeline") &&
                view.Find("timeline")->Find("tracks")) {
                const auto& tracks =
                    view.Find("timeline")->Find("tracks")->AsArray();
                allAudio = tracks.size() == 1;
                for (const mcp_json::Value& track : tracks)
                    allAudio = allAudio && track.Find("kind") &&
                               track.Find("kind")->AsString() == "audio";
            }
            Check(allAudio, "kinds keeps only the tracks of those kinds: " +
                                audioOnly.result_json.substr(0, 300));
            CheckFailureEnvelope(describe(R"({"only":["nope"]})"),
                                 "a section that does not exist",
                                 "ValidationFailed");
            CheckFailureEnvelope(describe(R"({"kinds":["motion"]})"),
                                 "a track kind that does not exist",
                                 "ValidationFailed");
        }

        Document liveDocument = fixture;
        EditLog liveLog;
        int switches = 0;
        McpLiveBackend live(liveDocument, liveLog, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr,
                            [&](const Ulid&, std::string&) {
                                ++switches;
                                return true;
                            });
        CheckFailureEnvelope(registry.Call(live, "timeline_stats", other),
                             "the app reading another timeline",
                             "UnknownSequence");
        mcp_json::Value plan = mcp_json::Value::MakeObject();
        plan.Set("timeline_id",
                 mcp_json::Value::MakeString("01K3S999999999999999999999"));
        plan.Set("label", mcp_json::Value::MakeString("x"));
        mcp_json::Value steps = mcp_json::Value::MakeArray();
        plan.Set("steps", steps);
        registry.Call(live, "prepare_edit_plan", plan);
        Check(switches == 0,
              "neither a read nor a plan switches the app's timeline");
    }

    IdResolver fixtureResolver(fixture);
    Ulid resolvedLinkGroup;
    std::string resolutionError;
    Check(fixtureResolver.Resolve("link_group_id", "01K30000000000000000000005",
                                  resolvedLinkGroup, resolutionError) &&
              resolvedLinkGroup == "01K30000000000000000000005",
          "ID resolver includes the shared IDs required by linked tools: " +
              resolutionError);
    Ulid resolvedAlias;
    Check(fixtureResolver.Resolve("clip_id", "A1", resolvedAlias,
                                  resolutionError) &&
              resolvedAlias == "01K30000000000000000000003",
          "ID resolver accepts the clip aliases exposed by describe: " +
              resolutionError);

    McpToolRegistry intentRegistry;
    InMemoryBackend intentBackend(fixture);
    mcp_json::Value previewArguments;
    std::string intentParseError;
    Check(
        mcp_json::Value::Parse(
            R"({"clip_id":"A1","edge":"Tail","amount":1,"unit":"Frames","preview":true})",
            previewArguments, intentParseError),
        "shorten intent preview arguments parse: " + intentParseError);
    const auto shortenTool = std::find_if(
        intentRegistry.Tools().begin(), intentRegistry.Tools().end(),
        [](const McpTool& tool) { return tool.name == "shorten_linked_clip"; });
    Check(shortenTool != intentRegistry.Tools().end() &&
              shortenTool->input_schema_json.find("timeline_id") !=
                  std::string::npos,
          "timeline editing schemas publish optional timeline_id");
    InMemoryBackend strictBackend(fixture);
    strictBackend.RequireExplicitTimeline();
    const McpToolCallOutcome strictRefusal = intentRegistry.Call(
        strictBackend, "shorten_linked_clip", previewArguments);
    CheckFailureEnvelope(strictRefusal, "strict edit without timeline",
                         "TimelineRequired");
    previewArguments.Set("timeline_id",
                         mcp_json::Value::MakeString(fixture.sequence.id));
    const McpToolCallOutcome strictExplicit = intentRegistry.Call(
        strictBackend, "shorten_linked_clip", previewArguments);
    Check(strictExplicit.ok, "strict edit accepts its explicit timeline: " +
                                 strictExplicit.message);
    // PRD17 -- ROADMAP.md. Strict mode holds for reads too: in a series the
    // active timeline is nobody's, and an agent decides its edits on what
    // it reads.
    CheckFailureEnvelope(intentRegistry.Call(strictBackend, "timeline_stats",
                                             mcp_json::Value::MakeObject()),
                         "strict read without timeline", "TimelineRequired");
    {
        mcp_json::Value named = mcp_json::Value::MakeObject();
        named.Set("timeline_id",
                  mcp_json::Value::MakeString(fixture.sequence.id));
        const McpToolCallOutcome read =
            intentRegistry.Call(strictBackend, "timeline_stats", named);
        Check(read.ok,
              "a strict read naming its timeline is answered: " + read.message);
    }
    // PRD40 -- ROADMAP.md. A rush is no timeline: read_frame on a media_id
    // alone passes strict mode, under either of its names (this backend
    // then refuses for want of a file, by another name); on a clip it still
    // names its timeline.
    for (const char* rushOnly : {R"({"media_id":"01K30000000000000000000001",)"
                                 R"("source_in":{"value":0,"rate":25}})",
                                 R"({"source_id":"01K30000000000000000000001",)"
                                 R"("source_in":{"value":0,"rate":25}})"}) {
        mcp_json::Value rushArguments;
        Check(mcp_json::Value::Parse(rushOnly, rushArguments, intentParseError),
              "rush read arguments parse");
        const McpToolCallOutcome rush =
            intentRegistry.Call(strictBackend, "read_frame", rushArguments);
        Check(rush.error_name != "TimelineRequired",
              "a strict read of a rush alone names no timeline: " +
                  rush.result_json);
    }
    {
        mcp_json::Value clipArguments;
        Check(mcp_json::Value::Parse(R"({"clip_id":"A1"})", clipArguments,
                                     intentParseError),
              "clip read arguments parse");
        CheckFailureEnvelope(
            intentRegistry.Call(strictBackend, "read_frame", clipArguments),
            "a strict read of a clip without timeline", "TimelineRequired");
    }
    previewArguments = mcp_json::Value::MakeObject();
    Check(
        mcp_json::Value::Parse(
            R"({"clip_id":"A1","edge":"Tail","amount":1,"unit":"Frames","preview":true})",
            previewArguments, intentParseError),
        "shorten preview arguments reset after strict check");
    const std::string intentBefore =
        intentBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome previewOutcome = intentRegistry.Call(
        intentBackend, "shorten_linked_clip", previewArguments);
    Check(previewOutcome.ok && previewOutcome.result_json.find(
                                   "\"preview\":true") != std::string::npos,
          "shorten_linked_clip previews one deterministic linked operation: " +
              previewOutcome.message);
    Check(intentBackend.CurrentDocument().SaveToString() == intentBefore,
          "shorten_linked_clip preview leaves the document byte-identical");

    previewArguments.Set("preview", mcp_json::Value::MakeBool(false));
    const McpToolCallOutcome shortenOutcome = intentRegistry.Call(
        intentBackend, "shorten_linked_clip", previewArguments);
    Check(shortenOutcome.ok,
          "shorten_linked_clip applies through the backend: " +
              shortenOutcome.message);
    Document intentExpected = fixture;
    EditLog intentExpectedLog;
    EditError intentExpectedError = EditError::None;
    std::string intentExpectedMessage;
    Check(intentExpectedLog.Apply(
              intentExpected,
              TrimLinkedClipsOperation{
                  "01K30000000000000000000005",
                  {{"01K30000000000000000000003", TrimEdge::Tail, {-1, 25}},
                   {"01K30000000000000000000004", TrimEdge::Tail, {-1, 25}}},
                  {}},
              intentExpectedError, intentExpectedMessage),
          "reference deterministic linked trim succeeds: " +
              intentExpectedMessage);
    Check(intentBackend.CurrentDocument().SaveToString() ==
              intentExpected.SaveToString(),
          "shorten_linked_clip computes IDs, sign and frame time exactly");

    InMemoryBackend backend(fixture);
    McpToolRegistry shortRegistry;

    // A6 -- both visual summaries return one MCP image with exact cell
    // metadata, without asking the caller to compute any timeline position.
    {
        const McpToolCallOutcome contact = shortRegistry.Call(
            backend, "contact_sheet", mcp_json::Value::MakeObject());
        Check(
            contact.ok && contact.image_mime == "image/jpeg" &&
                contact.image_base64 == "ZmFrZS1qcGVn" &&
                contact.result_json.find("\"role\":\"middle\"") !=
                    std::string::npos,
            "contact_sheet returns an image and exact middle-cell metadata: " +
                contact.message);

        Document adjacent = fixture;
        adjacent.sequence.tracks[0].clips[0].timeline_in = {0, 25};
        adjacent.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        InMemoryBackend cutBackend(adjacent);
        const McpToolCallOutcome cuts = shortRegistry.Call(
            cutBackend, "cut_sheet", mcp_json::Value::MakeObject());
        Check(cuts.ok && cuts.image_mime == "image/jpeg" &&
                  cuts.result_json.find("\"role\":\"before\"") !=
                      std::string::npos &&
                  cuts.result_json.find("\"role\":\"after\"") !=
                      std::string::npos &&
                  cuts.result_json.find(
                      "\"timeline_position\":{\"value\":9,\"rate\":25}") !=
                      std::string::npos,
              "cut_sheet returns exact adjacent frame pairs: " + cuts.message);

        mcp_json::Value invalid = mcp_json::Value::MakeObject();
        invalid.Set("max_images", mcp_json::Value::MakeInt(1));
        CheckFailureEnvelope(
            shortRegistry.Call(cutBackend, "cut_sheet", invalid),
            "cut sheet incomplete pair", "ValidationFailed");

        // DEC29 -- docs/roadmap/GRADE-2026-09.md. The sheets of an edit
        // take graded and hand it to the render; the reply says it. A
        // sheet of rushes refuses it by name.
        Check(
            !backend.CapturedSheetGraded() && !cutBackend.CapturedSheetGraded(),
            "DEC29: a sheet is ungraded by default");
        mcp_json::Value gradedArguments = mcp_json::Value::MakeObject();
        gradedArguments.Set("graded", mcp_json::Value::MakeBool(true));
        const McpToolCallOutcome gradedContact =
            shortRegistry.Call(backend, "contact_sheet", gradedArguments);
        Check(gradedContact.ok && backend.CapturedSheetGraded() &&
                  gradedContact.result_json.find("\"graded\":true") !=
                      std::string::npos,
              "DEC29: contact_sheet graded renders graded and says so: " +
                  gradedContact.message);
        const McpToolCallOutcome gradedCuts =
            shortRegistry.Call(cutBackend, "cut_sheet", gradedArguments);
        Check(gradedCuts.ok && cutBackend.CapturedSheetGraded(),
              "DEC29: cut_sheet graded renders graded: " + gradedCuts.message);
        {
            InMemoryBackend rushes(FixtureWithLibrary(fixture));
            mcp_json::Value rushArguments = mcp_json::Value::MakeObject();
            rushArguments.Set("media_id", mcp_json::Value::MakeString(
                                              "01K30000000000000000000001"));
            rushArguments.Set("graded", mcp_json::Value::MakeBool(true));
            const McpToolCallOutcome gradedRush =
                shortRegistry.Call(rushes, "media_sheet", rushArguments);
            CheckFailureEnvelope(gradedRush, "DEC29: media_sheet graded",
                                 "ValidationFailed");
            Check(gradedRush.message.find("a rush has no grade") !=
                          std::string::npos &&
                      !rushes.CapturedSheet().has_value(),
                  "DEC29: media_sheet says why it has no graded: " +
                      gradedRush.message);
        }

        // PRD39 -- ROADMAP.md. Nothing to show is refused, and says why,
        // before anything renders: the fixture's two shots are apart, and
        // an empty timeline has no shot. The render's IoError used to say
        // it, and this backend rendered an empty grid.
        InMemoryBackend apart(fixture);
        const McpToolCallOutcome uncut = shortRegistry.Call(
            apart, "cut_sheet", mcp_json::Value::MakeObject());
        CheckFailureEnvelope(uncut, "cut sheet without a cut",
                             "ValidationFailed");
        Check(uncut.message.rfind(
                  "nothing to show: the timeline has no visible cut", 0) == 0 &&
                  !apart.CapturedSheet().has_value(),
              "a timeline without a cut says so, and renders nothing: " +
                  uncut.message);
        Document empty = fixture;
        for (DocumentTrack& track : empty.sequence.tracks) track.clips.clear();
        InMemoryBackend emptyBackend(empty);
        const McpToolCallOutcome blank = shortRegistry.Call(
            emptyBackend, "contact_sheet", mcp_json::Value::MakeObject());
        CheckFailureEnvelope(blank, "contact sheet of nothing",
                             "ValidationFailed");
        Check(blank.message.rfind("nothing to show: the timeline has no "
                                  "visible video shot",
                                  0) == 0,
              "an empty timeline says so: " + blank.message);

        // PRD39 -- one default for a sheet of rushes, the engine's, for one
        // rush as for several, as --media-sheet reads it: one rush gave 12.
        for (const char* key : {"media_id", "media_ids"}) {
            InMemoryBackend rushes(FixtureWithLibrary(fixture));
            mcp_json::Value arguments = mcp_json::Value::MakeObject();
            const mcp_json::Value id =
                mcp_json::Value::MakeString("01K30000000000000000000001");
            if (std::string(key) == "media_id") {
                arguments.Set(key, id);
            } else {
                mcp_json::Value ids = mcp_json::Value::MakeArray();
                ids.Push(id);
                arguments.Set(key, std::move(ids));
            }
            const McpToolCallOutcome sheet =
                shortRegistry.Call(rushes, "media_sheet", arguments);
            Check(sheet.ok && rushes.CapturedSheet().has_value() &&
                      rushes.CapturedSheet()->frames.size() ==
                          static_cast<size_t>(
                              TimelineSheetSettings{}.maximum_images) &&
                      TimelineSheetSettings{}.maximum_images == 24,
                  std::string("media_sheet with ") + key +
                      " gives the engine's 24 cells: " + sheet.message);
        }
    }

    // B11 -- image tools reject audio-only media by capability before a
    // backend starts FFmpeg and returns a generic I/O failure.
    {
        Document audioOnly;
        LibraryMedia media;
        media.id = "01K31000000000000000000001";
        media.path = "voice.wav";
        media.filename = "voice.wav";
        media.codec = "pcm_s16le";
        media.has_video = false;
        media.width = 0;
        media.height = 0;
        media.rate = {48000, 1};
        media.duration = {48000, 48000};
        media.orientation = "audio";
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 1;
        audioOnly.library.push_back(media);
        audioOnly.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        InMemoryBackend audioBackend(audioOnly);
        McpToolRegistry audioRegistry;
        mcp_json::Value frameArguments;
        std::string frameParseError;
        Check(
            mcp_json::Value::Parse(
                R"({"media_id":"01K31000000000000000000001","source_in":{"value":0,"rate":48000}})",
                frameArguments, frameParseError),
            "audio-only frame arguments parse: " + frameParseError);
        const McpToolCallOutcome frameOutcome =
            audioRegistry.Call(audioBackend, "read_frame", frameArguments);
        // G1b -- a media has no grade until a clip places it.
        mcp_json::Value gradedMedia;
        Check(
            mcp_json::Value::Parse(
                R"({"media_id":"01K31000000000000000000001","source_in":{"value":0,"rate":48000},"graded":true})",
                gradedMedia, frameParseError),
            "graded media arguments parse: " + frameParseError);
        CheckFailureEnvelope(
            audioRegistry.Call(audioBackend, "read_frame", gradedMedia),
            "read_frame graded on a media", "ValidationFailed");
        CheckFailureEnvelope(frameOutcome, "read_frame on audio-only media",
                             "InvalidOperation");
        Check(frameOutcome.message.find("audio-only") != std::string::npos,
              "the frame refusal names the missing picture capability");
    }

    // G1 -- docs/roadmap/GRADE-2026-09.md. measure_color reads its
    // arguments exactly, refuses what the measure cannot mean, and writes
    // the engine's numbers as exact decimals.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  "measure_color arguments parse: " + parseError);
            return registry.Call(backend, "measure_color", arguments);
        };
        const McpToolCallOutcome measured = call(
            R"({"clip_ids":["01K30000000000000000000003"],"position":"End",)"
            R"("region":{"x":{"num":1,"den":4},"y":{"num":0,"den":1},)"
            R"("width":{"num":1,"den":2},"height":{"num":1,"den":1}}})");
        Check(measured.ok && backend.MeasuredPlan().has_value(),
              "measure_color reaches the backend: " + measured.message);
        if (backend.MeasuredPlan()) {
            const ColorMeasurePlan& plan = *backend.MeasuredPlan();
            Check(plan.clip_ids.size() == 1 && plan.position == "End" &&
                      plan.region.present && plan.region.x.num == 1 &&
                      plan.region.x.den == 4 && plan.region.width.den == 2,
                  "the plan carries the clips, position and exact region");
        }
        Check(measured.result_json.find("\"p50\":41.5") != std::string::npos &&
                  measured.result_json.find("\"position\":\"End\"") !=
                      std::string::npos &&
                  measured.result_json.find("\"hue_degrees\"") !=
                      std::string::npos &&
                  measured.result_json.find("\"scene\":null") !=
                      std::string::npos,
              "the reply writes 4150 hundredths as 41.5, and no scene "
              "without colour management: " +
                  measured.result_json);

        CheckFailureEnvelope(call(R"({"clip_ids":[]})"),
                             "measure_color without clips", "ValidationFailed");

        // AGENT-2026-09 A16 -- propose_exposure: refused without colour
        // management; with it, every video clip measured by the same path
        // and one step per clip off the target.
        {
            const auto propose = [&](InMemoryBackend& target,
                                     const std::string& json) {
                mcp_json::Value arguments;
                std::string parseFailure;
                Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                      "propose_exposure arguments parse: " + parseFailure);
                return registry.Call(target, "propose_exposure", arguments);
            };
            CheckFailureEnvelope(propose(backend, "{}"),
                                 "propose_exposure without colour management",
                                 "InvalidOperation");
            Document managed = fixture;
            managed.color_management.enabled = true;
            managed.color_management.input_transfer = "hlg";
            managed.color_management.input_gamut = "rec2020";
            InMemoryBackend managedBackend(managed);
            managedBackend.SetMeasuredScene(197);
            const McpToolCallOutcome proposed =
                propose(managedBackend, R"({"tolerance_stops":0.25})");
            mcp_json::Value view;
            std::string parseError;
            Check(proposed.ok &&
                      mcp_json::Value::Parse(proposed.result_json, view,
                                             parseError) &&
                      managedBackend.MeasuredPlan() &&
                      managedBackend.MeasuredPlan()->clip_ids.size() == 2 &&
                      view.Find("proposed")->Dump() == "2" &&
                      view.Find("steps")->AsArray().size() == 2 &&
                      view.Find("tolerance_stops")->Dump() == "0.25" &&
                      view.Find("clips")
                              ->AsArray()[0]
                              .Find("correction_stops")
                              ->Dump() == "-1.97",
                  "propose_exposure measures every video clip and proposes "
                  "a step for each: " +
                      proposed.result_json);
            const McpToolCallOutcome tolerated =
                propose(managedBackend, R"({"target_stops":2})");
            Check(tolerated.ok &&
                      mcp_json::Value::Parse(tolerated.result_json, view,
                                             parseError) &&
                      view.Find("proposed")->Dump() == "0" &&
                      view.Find("clips")
                              ->AsArray()[0]
                              .Find("reason")
                              ->AsString() == "within_tolerance",
                  "a target near the measure proposes nothing: " +
                      tolerated.result_json);
            CheckFailureEnvelope(
                propose(managedBackend, R"({"target_stops":"deux"})"),
                "a target that is not a number", "ValidationFailed");
            CheckFailureEnvelope(
                propose(managedBackend, R"({"tolerance_stops":-1})"),
                "a negative tolerance", "ValidationFailed");

            // AGENT-2026-09 A20 -- propose_grade: the same refusals, the
            // anchor and the aspects read into the plan, the face target by
            // default, no balance on a whole frame.
            const auto grade = [&](InMemoryBackend& target,
                                   const std::string& json) {
                mcp_json::Value arguments;
                std::string parseFailure;
                Check(mcp_json::Value::Parse(json, arguments, parseFailure),
                      "propose_grade arguments parse: " + parseFailure);
                return registry.Call(target, "propose_grade", arguments);
            };
            CheckFailureEnvelope(grade(backend, "{}"),
                                 "propose_grade without colour management",
                                 "InvalidOperation");
            const McpToolCallOutcome faced = grade(managedBackend, "{}");
            Check(
                faced.ok &&
                    mcp_json::Value::Parse(faced.result_json, view,
                                           parseError) &&
                    managedBackend.GradedPlan() &&
                    managedBackend.GradedPlan()->anchor == "face" &&
                    managedBackend.GradedPlan()->exposure &&
                    managedBackend.GradedPlan()->balance &&
                    managedBackend.GradedPlan()->target_hundredths == 70 &&
                    managedBackend.GradedPlan()->tolerance_hundredths == 25 &&
                    managedBackend.GradedPlan()->highlight_ceiling_hundredths ==
                        250 &&
                    managedBackend.GradedPlan()->clip_ids.size() == 2 &&
                    view.Find("anchor")->AsString() == "face" &&
                    view.Find("target_stops")->Dump() == "0.7" &&
                    view.Find("proposed")->Dump() == "1" &&
                    // AX-2026-09 -- the steps and the knobs come with
                    // verbose: true; apply: true does what they are for.
                    view.Find("steps") == nullptr &&
                    view.Find("clips")->AsArray()[0].Find("knobs") == nullptr &&
                    view.Find("clips")
                            ->AsArray()[1]
                            .Find("reason")
                            ->AsString() == "no_face",
                "propose_grade reads every video clip on its face, aimed "
                "at +0.7 by default: " +
                    faced.result_json);
            const McpToolCallOutcome neutral =
                grade(managedBackend,
                      R"({"anchor":"neutral","region":{"x":{"num":1,"den":4},)"
                      R"("y":{"num":1,"den":4},"width":{"num":1,"den":2},)"
                      R"("height":{"num":1,"den":2}},"aspects":["balance"]})");
            Check(neutral.ok &&
                      mcp_json::Value::Parse(neutral.result_json, view,
                                             parseError) &&
                      managedBackend.GradedPlan()->anchor == "neutral" &&
                      managedBackend.GradedPlan()->region.present &&
                      !managedBackend.GradedPlan()->exposure &&
                      managedBackend.GradedPlan()->balance &&
                      managedBackend.GradedPlan()->target_hundredths == 0 &&
                      view.Find("region") != nullptr &&
                      view.Find("proposed")->Dump() == "2",
                  "a neutral patch anchors the balance alone, at 0 stops: " +
                      neutral.result_json);
            const McpToolCallOutcome median =
                grade(managedBackend,
                      R"({"anchor":"median","highlight_ceiling_stops":3})");
            Check(
                median.ok && managedBackend.GradedPlan()->anchor == "median" &&
                    managedBackend.GradedPlan()->exposure &&
                    !managedBackend.GradedPlan()->balance &&
                    managedBackend.GradedPlan()->highlight_ceiling_hundredths ==
                        300,
                "a whole frame has no balance to read, and the ceiling is "
                "the caller's: " +
                    median.result_json);
            Check(!managedBackend.GradedPlan()->display_ceiling,
                  "an SDR sequence keeps the scene ceiling");
            // GR15 -- an HLG sequence: the face aimed at +1.5 and the
            // ceiling taken from the output's clipping, unless the caller
            // names either.
            {
                Document hlg = managed;
                hlg.color_management.output_gamut = "rec2020";
                hlg.color_management.output_transfer = "hlg";
                InMemoryBackend hlgBackend(hlg);
                const McpToolCallOutcome lifted = grade(hlgBackend, "{}");
                Check(lifted.ok &&
                          mcp_json::Value::Parse(lifted.result_json, view,
                                                 parseError) &&
                          hlgBackend.GradedPlan() &&
                          hlgBackend.GradedPlan()->target_hundredths == 150 &&
                          hlgBackend.GradedPlan()->display_ceiling &&
                          view.Find("target_stops")->Dump() == "1.5" &&
                          view.Find("highlight_ceiling") != nullptr &&
                          view.Find("highlight_ceiling")->AsString() ==
                              "display_clipping" &&
                          view.Find("highlight_ceiling_stops") == nullptr,
                      "an HLG sequence aims the face at +1.5 under the "
                      "output's clipping: " +
                          lifted.result_json.substr(0, 300));
                const McpToolCallOutcome named =
                    grade(hlgBackend,
                          R"({"target_stops":1,"highlight_ceiling_stops":3})");
                Check(
                    named.ok &&
                        hlgBackend.GradedPlan()->target_hundredths == 100 &&
                        !hlgBackend.GradedPlan()->display_ceiling &&
                        hlgBackend.GradedPlan()->highlight_ceiling_hundredths ==
                            300,
                    "what the caller names stays, HLG or not");
                const McpToolCallOutcome patch = grade(
                    hlgBackend,
                    R"({"anchor":"neutral","region":{"x":{"num":1,"den":4},)"
                    R"("y":{"num":1,"den":4},"width":{"num":1,"den":2},)"
                    R"("height":{"num":1,"den":2}}})");
                Check(patch.ok &&
                          hlgBackend.GradedPlan()->target_hundredths == 0 &&
                          hlgBackend.GradedPlan()->display_ceiling,
                      "a grey patch still lands on grey, under the output's "
                      "clipping");
            }
            CheckFailureEnvelope(
                grade(managedBackend, R"({"anchor":"neutral"})"),
                "a neutral anchor without its patch", "ValidationFailed");
            CheckFailureEnvelope(grade(managedBackend, R"({"anchor":"sky"})"),
                                 "an unknown anchor", "ValidationFailed");
            CheckFailureEnvelope(
                grade(managedBackend, R"({"aspects":["contrast"]})"),
                "an unknown aspect", "ValidationFailed");
            CheckFailureEnvelope(grade(managedBackend, R"({"aspects":[]})"),
                                 "no aspect at all", "ValidationFailed");
            CheckFailureEnvelope(
                grade(managedBackend, R"({"tolerance_stops":-1})"),
                "a negative grade tolerance", "ValidationFailed");
            CheckFailureEnvelope(
                grade(managedBackend,
                      R"({"clip_ids":["01K3000000000000000000FFFF"]})"),
                "propose_grade on an unknown clip", "ValidationFailed");
            // AGENT-2026-09 A41 -- the reading is brief unless asked, and
            // apply:true sets the proposed stacks in one call.
            const McpToolCallOutcome brief = grade(managedBackend, "{}");
            Check(brief.ok &&
                      mcp_json::Value::Parse(brief.result_json, view,
                                             parseError) &&
                      view.Find("clips")->AsArray()[0].Find("before") &&
                      view.Find("clips")->AsArray()[0].Find("before")->Find(
                          "anchor_warmth") == nullptr &&
                      view.Find("steps") == nullptr,
                  "the default reading leaves the zones out: " +
                      brief.result_json.substr(0, 200));
            // AX-2026-09 -- the knobs and the steps come when asked.
            const McpToolCallOutcome full =
                grade(managedBackend, R"({"verbose":true})");
            Check(full.ok &&
                      mcp_json::Value::Parse(full.result_json, view,
                                             parseError) &&
                      view.Find("steps") != nullptr &&
                      view.Find("steps")->AsArray().size() == 1 &&
                      view.Find("clips")->AsArray()[0].Find("knobs") != nullptr,
                  "verbose: true lists the knobs and the steps: " +
                      full.result_json.substr(0, 200));
            const McpToolCallOutcome applied =
                grade(managedBackend, R"({"apply":true})");
            Check(applied.ok &&
                      mcp_json::Value::Parse(applied.result_json, view,
                                             parseError) &&
                      view.Find("applied")->Dump() == "1" &&
                      view.Find("steps") == nullptr &&
                      managedBackend.CurrentDocument()
                              .FindClip("01K30000000000000000000003")
                              ->effects.size() == 1,
                  "apply:true sets the proposed stack on the clip and drops "
                  "the steps from the reply: " +
                      applied.result_json.substr(0, 300));
        }
        CheckFailureEnvelope(
            call(R"({"clip_ids":["01K30000000000000000000003"],)"
                 R"("position":"Nowhere"})"),
            "measure_color at an unknown position", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_ids":["01K30000000000000000000003"],"region":{)"
                 R"("x":{"num":1,"den":2},"y":{"num":0,"den":1},)"
                 R"("width":{"num":2,"den":3},"height":{"num":1,"den":1}}})"),
            "measure_color on a region past the edge", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_ids":["01K30000000000000000000003"],"region":{)"
                 R"("x":{"num":0,"den":1},"y":{"num":0,"den":1},)"
                 R"("width":{"num":1,"den":2}}})"),
            "measure_color on a region without its height", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_ids":["01K3000000000000000000FFFF"]})"),
            "measure_color on an unknown clip");
    }

    // PL2 to PL4 -- docs/roadmap/PLANS-2026-10.md. A rush described once,
    // searched afterwards, and what still has no description said where
    // the agent looks.
    {
        Document described = fixture;
        described.library.clear();
        const auto rush = [](const Ulid& id, const std::string& name) {
            LibraryMedia media;
            media.id = id;
            media.path = "rushes/" + name;
            media.filename = name;
            media.codec = "h264";
            media.width = 1920;
            media.height = 1080;
            media.orientation = "landscape";
            media.rate = {25, 1};
            media.duration = {250, 25};
            return media;
        };
        const Ulid yard = "01K7P000000000000000000001";
        const Ulid street = "01K7P000000000000000000002";
        described.library.push_back(rush(yard, "COUR.MP4"));
        described.library.push_back(rush(street, "RUE.MP4"));
        described.sources.push_back(
            {yard, "rushes/COUR.MP4", {25, 1}, {250, 25}});
        described.sources.push_back(
            {street, "rushes/RUE.MP4", {25, 1}, {250, 25}});
        McpToolRegistry registry;
        InMemoryBackend backend(described);
        const auto call = [&](const std::string& tool,
                              const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const auto media = [&](const Ulid& id) {
            return *backend.CurrentDocument().FindLibraryMedia(id);
        };
        const std::string original = backend.CurrentDocument().SaveToString();

        const McpToolCallOutcome written = call(
            "describe_shots",
            R"({"media_id":"01K7P000000000000000000001","author":"claude",)"
            R"("described_at":"2026-10-21T10:00:00Z","shots":[)"
            R"({"shot_id":"01K7P0000000000000000000S1",)"
            R"("source_in":{"value":0,"rate":25},)"
            R"("duration":{"value":100,"rate":25},"size":"WIDE",)"
            R"("movement":"static","subjects":["pêcheurs"],)"
            R"("action":"remontent les filets","keywords":["marée"]},)"
            R"({"source_in":{"value":150,"rate":25},)"
            R"("duration":{"value":50,"rate":25},"size":"insert",)"
            R"("movement":"handheld","subjects":["mains","clavier"],)"
            R"("quality":"usable","reserve":"flou au début"}]})");
        Check(written.ok && media(yard).shots.size() == 2 &&
                  media(yard).shots[0].size == ShotSize::Wide &&
                  media(yard).shots[0].author == "claude" &&
                  media(yard).shots[1].quality == ShotQuality::Usable &&
                  written.result_json.find("\"described_shots\":2") !=
                      std::string::npos,
              "describe_shots writes two stretches, the size in any case: " +
                  written.message + written.result_json);
        Check(call("undo", "{}").ok &&
                  backend.CurrentDocument().SaveToString() == original,
              "one undo step takes both back");
        Check(call("redo", "{}").ok && media(yard).shots.size() == 2,
              "and one redo puts them back");

        const McpToolCallOutcome overlapping = call(
            "describe_shots",
            R"({"media_id":"01K7P000000000000000000001","author":"claude",)"
            R"("shots":[{"source_in":{"value":50,"rate":25},)"
            R"("duration":{"value":75,"rate":25},"size":"close",)"
            R"("movement":"static"}]})");
        Check(!overlapping.ok &&
                  overlapping.message.find(
                      "overlaps shot '01K7P0000000000000000000S1'") !=
                      std::string::npos &&
                  media(yard).shots.size() == 2,
              "a stretch over a standing description is refused, naming "
              "it: " +
                  overlapping.message);
        Check(!call("describe_shots",
                    R"({"media_id":"01K7P000000000000000000001",)"
                    R"("shots":[{"source_in":{"value":200,"rate":25},)"
                    R"("duration":{"value":25,"rate":25},"size":"close",)"
                    R"("movement":"static"}]})")
                   .ok,
              "an author is required");
        Check(!call("describe_shots",
                    R"({"media_id":"01K7P000000000000000000001",)"
                    R"("author":"claude","shots":[{"source_in":)"
                    R"({"value":200,"rate":25},"duration":{"value":25,)"
                    R"("rate":25},"size":"gros plan","movement":"static"}]})")
                   .ok,
              "a size outside the vocabulary is refused");
        const McpToolCallOutcome replaced =
            call("describe_shots",
                 R"({"media_id":"01K7P000000000000000000001","author":"Nadia",)"
                 R"("author_kind":"person","shots":[)"
                 R"({"shot_id":"01K7P0000000000000000000S1",)"
                 R"("source_in":{"value":0,"rate":25},)"
                 R"("duration":{"value":100,"rate":25},"size":"extreme_wide",)"
                 R"("movement":"pan","subjects":["pêcheurs"]}]})");
        Check(replaced.ok &&
                  media(yard).shots[0].size == ShotSize::ExtremeWide &&
                  media(yard).shots[0].author_kind == ShotAuthorKind::Person &&
                  media(yard).shots[0].described_at.size() == 20,
              "the same shot_id replaces, and the time defaults to now: " +
                  replaced.message);

        const McpToolCallOutcome found = call(
            "search_shots", R"({"text":"PECHEUR","sizes":["extreme_wide"]})");
        Check(found.ok &&
                  found.result_json.find("\"total\":1") != std::string::npos &&
                  found.result_json.find(
                      "\"shot_id\":\"01K7P0000000000000000000S1\"") !=
                      std::string::npos &&
                  found.result_json.find(
                      "\"read_frame\":{\"media_id\":"
                      "\"01K7P000000000000000000001\",\"source_in\":"
                      "{\"value\":49,\"rate\":25}}") != std::string::npos &&
                  found.result_json.find("\"undescribed_media\":[{\"media_id\":"
                                         "\"01K7P000000000000000000002\"") !=
                      std::string::npos,
              "search_shots finds by word and size, says where to look and "
              "which rush it could not see: " +
                  found.message + found.result_json);
        const McpToolCallOutcome none =
            call("search_shots", R"({"qualities":["poor"]})");
        Check(none.ok &&
                  none.result_json.find("\"total\":0") != std::string::npos,
              "a filter nothing passes answers an empty list: " +
                  none.result_json);
        Check(!call("search_shots", R"({"sizes":["tight"]})").ok,
              "a filter outside the vocabulary is refused");

        // The same text describe answers (this backend's describe reads the
        // project it was built from, not the edits applied since).
        const std::string described_view =
            DescribeDocument(backend.CurrentDocument());
        Check(
            described_view.find("\"described_shots\":2") != std::string::npos &&
                described_view.find("\"described_shots\":0") !=
                    std::string::npos,
            "describe counts the described stretches of each rush");
        const McpToolCallOutcome sheet =
            call("media_sheet", R"({"media_id":"01K7P000000000000000000001",)"
                                R"("max_images":5})");
        Check(sheet.ok && backend.CapturedSheet()->frames.size() == 5 &&
                  backend.CapturedSheet()->frames[0].shot_id ==
                      "01K7P0000000000000000000S1" &&
                  backend.CapturedSheet()->frames[2].shot_id.empty(),
              "media_sheet marks which cells a description covers: " +
                  sheet.message);
        Check(call("describe_shots",
                   R"({"media_id":"01K7P000000000000000000001",)"
                   R"("remove_shot_ids":["01K7P0000000000000000000S1"]})")
                      .ok &&
                  media(yard).shots.size() == 1,
              "remove_shot_ids drops a description");
    }

    // G7, G3 -- docs/roadmap/GRADE-2026-09.md. A media's white balance and
    // colour input: set by hand, read again from each file, or given back to
    // the project; one undo step for all the media named.
    {
        Document colored = fixture;
        colored.library.clear();
        const auto rush = [](const Ulid& id, const std::string& name) {
            LibraryMedia media;
            media.id = id;
            media.path = "rushes/" + name;
            media.filename = name;
            media.codec = "h264";
            media.width = 1920;
            media.height = 1080;
            media.orientation = "landscape";
            media.rate = {25, 1};
            media.duration = {100, 25};
            return media;
        };
        const Ulid fx30 = "01K7C000000000000000000001";
        const Ulid phone = "01K7C000000000000000000002";
        const Ulid untagged = "01K7C000000000000000000003";
        colored.library.push_back(rush(fx30, "C0001.MP4"));
        colored.library.push_back(rush(phone, "IMG_0001.MOV"));
        colored.library.push_back(rush(untagged, "screen.mp4"));
        McpToolRegistry registry;
        InMemoryBackend backend(colored);
        backend.file_colors[fx30] = {
            4500, {"sony_sgamut3_cine", "sony_slog3", "auto", "auto"}};
        backend.file_colors[phone] = {0, {"rec709", "rec709", "auto", "auto"}};
        backend.file_colors[untagged] = {};
        const auto call = [&](const std::string& tool,
                              const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const auto media = [&](const Ulid& id) {
            return *backend.CurrentDocument().FindLibraryMedia(id);
        };
        const std::string original = backend.CurrentDocument().SaveToString();

        const McpToolCallOutcome read = call(
            "set_media_color_input",
            R"({"media_ids":["01K7C000000000000000000001",)"
            R"("01K7C000000000000000000002","01K7C000000000000000000003"],)"
            R"("from_file":true})");
        Check(read.ok && media(fx30).color_input.transfer == "sony_slog3" &&
                  media(phone).color_input.transfer == "rec709" &&
                  !DeclaresColorInput(media(untagged).color_input) &&
                  read.result_json.find("\"read\":[") != std::string::npos,
              "from_file declares what each file says, and leaves the "
              "untagged one to the project: " +
                  read.message + read.result_json);
        Check(call("undo", "{}").ok &&
                  backend.CurrentDocument().SaveToString() == original,
              "the three declarations are one undo step");
        Check(call("redo", "{}").ok &&
                  media(phone).color_input.transfer == "rec709",
              "and one redo step");

        Check(call("set_media_color_input",
                   R"({"media_ids":["01K7C000000000000000000003"],)"
                   R"("input_gamut":"rec2020","input_transfer":"hlg",)"
                   R"("input_range":"limited"})")
                      .ok &&
                  media(untagged).color_input.transfer == "hlg" &&
                  media(untagged).color_input.ycbcr_matrix == "auto" &&
                  media(untagged).color_input.range == "limited",
              "a declaration by hand, the matrix left to the file");
        Check(call("set_media_color_input",
                   R"({"media_ids":["01K7C000000000000000000002"],)"
                   R"("follow_project":true})")
                      .ok &&
                  !DeclaresColorInput(media(phone).color_input),
              "follow_project removes the declaration");

        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("input_gamut":"rec709","input_transfer":"hlg"})"),
            "an HLG picture outside BT.2020", "ValidationFailed");
        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("input_transfer":"rec709"})"),
            "a transfer without a gamut", "ValidationFailed");
        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("from_file":true,"follow_project":true})"),
            "two modes at once", "ValidationFailed");
        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"]})"),
            "no mode at all", "ValidationFailed");

        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A graphic is
        // declared by name, with its own matrix and range if need be, and
        // never with a camera curve.
        const McpToolCallOutcome graphic =
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("graphics":true,"input_range":"full"})");
        Check(graphic.ok && media(phone).color_input.graphics &&
                  media(phone).color_input.transfer.empty() &&
                  media(phone).color_input.range == "full" &&
                  media(phone).color_input.ycbcr_matrix == "auto",
              "DEC31: graphics declares the media a graphic: " +
                  graphic.message + graphic.result_json);
        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("graphics":true,"input_gamut":"rec709",)"
                 R"("input_transfer":"rec709"})"),
            "DEC31: a graphic with a camera curve", "ValidationFailed");
        CheckFailureEnvelope(
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("graphics":true,"follow_project":true})"),
            "DEC31: a graphic and the project's input at once",
            "ValidationFailed");
        Check(call("set_media_color_input",
                   R"({"media_ids":["01K7C000000000000000000002"],)"
                   R"("follow_project":true})")
                      .ok &&
                  !media(phone).color_input.graphics,
              "DEC31: follow_project undeclares the graphic");

        const McpToolCallOutcome balanced =
            call("set_media_white_balance",
                 R"({"media_ids":["01K7C000000000000000000001",)"
                 R"("01K7C000000000000000000002"],"from_camera":true})");
        Check(balanced.ok && media(fx30).white_balance_kelvin == 4500 &&
                  media(phone).white_balance_kelvin == 0,
              "from_camera reads the FX30's 4500 K and leaves the phone "
              "unknown: " +
                  balanced.message + balanced.result_json);
        Check(call("set_media_white_balance",
                   R"({"media_ids":["01K7C000000000000000000002"],)"
                   R"("kelvin":5600})")
                      .ok &&
                  media(phone).white_balance_kelvin == 5600,
              "a white balance by hand");
        CheckFailureEnvelope(
            call("set_media_white_balance",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("kelvin":500})"),
            "500 K", "ValidationFailed");
        backend.file_colors.erase(phone);
        const McpToolCallOutcome unreadable =
            call("set_media_color_input",
                 R"({"media_ids":["01K7C000000000000000000002"],)"
                 R"("from_file":true})");
        Check(!unreadable.ok && unreadable.message.find("no file for media") !=
                                    std::string::npos,
              "a file that cannot be read is an error, not a silence: " +
                  unreadable.message);
    }

    // G4 -- docs/roadmap/GRADE-2026-09.md. export_cdl folds a clip's
    // primary into a CDL or refuses it by stage; apply_cdl puts one in
    // place of clips' primaries, from a file or from Resolve's strings, on
    // the space its author declares, in one undo step.
    {
        Document graded = fixture;
        ClipEffect exposure;
        exposure.type = "color.exposure";
        exposure.params["amount"] = {1, 10};
        ClipEffect saturation;
        saturation.type = "color.saturation";
        saturation.params["amount"] = {1, 2};
        ClipEffect temperature;
        temperature.type = "color.temperature";
        temperature.params["kelvin"] = {3200, 1};
        ClipEffect secondExposure = exposure;
        secondExposure.id = GenerateUlid();
        graded.sequence.tracks[0].clips[0].effects = {exposure, saturation};
        graded.sequence.tracks[0].clips[1].effects = {temperature,
                                                      secondExposure};
        McpToolRegistry registry;
        InMemoryBackend backend(graded);
        const auto call = [&](const std::string& tool,
                              const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const auto effectsOf = [&](int index) {
            return backend.CurrentDocument()
                .sequence.tracks[0]
                .clips[static_cast<size_t>(index)]
                .effects;
        };
        const auto directory = std::filesystem::temp_directory_path() /
                               ("cutmachine-mcp-cdl-" + GenerateUlid());
        std::filesystem::create_directories(directory);
        const std::string collection = (directory / "look.ccc").string();

        const McpToolCallOutcome exported =
            call("export_cdl",
                 R"({"clip_ids":["01K30000000000000000000003"],"path":")" +
                     collection + R"(","resolve_node":2})");
        Check(exported.ok &&
                  exported.result_json.find(R"("space":"display")") !=
                      std::string::npos &&
                  exported.result_json.find(R"("Offset":"0.005708 0.005708 )"
                                            R"(0.005708")") !=
                      std::string::npos &&
                  exported.result_json.find(R"("Saturation":"1.5")") !=
                      std::string::npos &&
                  exported.result_json.find(R"("NodeIndex":"2")") !=
                      std::string::npos &&
                  std::filesystem::exists(collection),
              "export_cdl folds exposure and saturation, writes the file and "
              "Resolve's map: " +
                  exported.message + exported.result_json);
        // GR12, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A
        // saturation of 1.5 renders 9 codes apart in a DaVinci YRGB
        // project: the reply says so, by clip.
        Check(exported.result_json.find(
                  R"("warning_clip_ids":["01K30000000000000000000003"])") !=
                      std::string::npos &&
                  exported.result_json.find(
                      R"("resolve_yrgb_gap_codes":9.1)") != std::string::npos,
              "GR12: export_cdl warns of the YRGB saturation: " +
                  exported.result_json);
        // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. One
        // clip's grade pasted onto others, the sound skipped, one undo.
        {
            const std::string beforePaste =
                backend.CurrentDocument().SaveToString();
            const McpToolCallOutcome pasted =
                call("paste_clip_grade",
                     R"({"from_clip_id":"01K30000000000000000000003",)"
                     R"("clip_ids":["01K30000000000000000000004",)"
                     R"("01K30000000000000000000006"]})");
            const auto pastedStack = effectsOf(1);
            Check(pasted.ok &&
                      pasted.result_json.find(
                          R"("pasted":["01K30000000000000000000004"])") !=
                          std::string::npos &&
                      pasted.result_json.find(
                          R"("clip_id":"01K30000000000000000000006")") !=
                          std::string::npos &&
                      pastedStack.size() == 2 &&
                      pastedStack[0].type == "color.exposure" &&
                      pastedStack[1].type == "color.saturation",
                  "GR11: paste_clip_grade copies the stack and skips the "
                  "sound: " +
                      pasted.message + pasted.result_json);
            Check(call("undo", "{}").ok &&
                      backend.CurrentDocument().SaveToString() == beforePaste,
                  "GR11: one undo step");
            CheckFailureEnvelope(
                call("paste_clip_grade",
                     R"({"from_clip_id":"01K30000000000000000000006",)"
                     R"("clip_ids":["01K30000000000000000000004"]})"),
                "GR11: a sound has no grade to copy", "ValidationFailed");
        }
        const McpToolCallOutcome warm = call(
            "export_cdl", R"({"clip_ids":["01K30000000000000000000004"]})");
        Check(!warm.ok &&
                  warm.message.find("color.temperature") != std::string::npos,
              "a white balance refuses the clip by name: " + warm.message);
        CheckFailureEnvelope(
            call("export_cdl",
                 R"({"clip_ids":["01K30000000000000000000006"]})"),
            "export_cdl on an audio clip", "ValidationFailed");

        const std::string before = backend.CurrentDocument().SaveToString();
        const McpToolCallOutcome previewed =
            call("apply_cdl",
                 R"({"clip_ids":["01K30000000000000000000003",)"
                 R"("01K30000000000000000000004"],"space":"display",)"
                 R"("cdl":{"slope":"1.1 1 0.9","offset":"0 0.01 0",)"
                 R"("power":"1 1 1","saturation":"0.8"},"preview":true})");
        Check(previewed.ok &&
                  previewed.result_json.find(R"("applied":false)") !=
                      std::string::npos &&
                  previewed.result_json.find(R"("tool":"set_clip_effects")") !=
                      std::string::npos &&
                  backend.CurrentDocument().SaveToString() == before,
              "preview returns the steps and changes nothing: " +
                  previewed.message);
        const McpToolCallOutcome applied = call(
            "apply_cdl", R"({"clip_ids":["01K30000000000000000000003",)"
                         R"("01K30000000000000000000004"],"space":"display",)"
                         R"("cdl":{"slope":"1.1 1 0.9","offset":"0 0.01 0",)"
                         R"("power":"1 1 1","saturation":"0.8"}})");
        const auto first = effectsOf(0);
        const auto second = effectsOf(1);
        Check(
            applied.ok && first.size() == 2 &&
                first[0].type == "color.wheels" &&
                first[0].params.at("lift_g").num == 1 &&
                first[0].params.at("lift_g").den == 100 &&
                first[1].type == "color.saturation" &&
                first[1].params.at("amount").num == -1 &&
                first[1].params.at("amount").den == 5 &&
                first[1].id == graded.sequence.tracks[0].clips[0].effects[1].id,
            "the CDL replaces the first clip's primary, offset and "
            "saturation exact, the saturation stage kept by id: " +
                applied.message);
        Check(second.size() == 3 && second[0].type == "color.temperature" &&
                  second[1].type == "color.wheels",
              "the second keeps its white balance, the CDL where its "
              "exposure stood");
        const McpToolCallOutcome undone = call("undo", "{}");
        const std::string after = backend.CurrentDocument().SaveToString();
        Check(undone.ok && after == before,
              "both clips come back in one undo step");

        const McpToolCallOutcome fromFile =
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000004"],)"
                              R"("space":"display","path":")" +
                                  collection + R"("})");
        Check(
            fromFile.ok && effectsOf(1).size() == 3 &&
                fromFile.result_json.find(R"("file_input_description":)"
                                          R"("Display")") != std::string::npos,
            "a CDL read back from the file applies, and says the space the "
            "file names: " +
                fromFile.message + fromFile.result_json);

        // A collection of two, as a colourist sends a scene.
        std::ofstream(directory / "scene.ccc")
            << R"(<ColorCorrectionCollection xmlns="urn:ASC:CDL:v1.01">
<ColorCorrection id="wide"><SOPNode><Slope>1 1 1</Slope><Offset>0 0 0</Offset><Power>1 1 1</Power></SOPNode><SatNode><Saturation>1.1</Saturation></SatNode></ColorCorrection>
<ColorCorrection id="close"><SOPNode><Slope>1.02 1 1</Slope><Offset>0 0 0</Offset><Power>1 1 1</Power></SOPNode><SatNode><Saturation>0.9</Saturation></SatNode></ColorCorrection>
</ColorCorrectionCollection>
)";
        const std::string scene = (directory / "scene.ccc").string();
        const McpToolCallOutcome ambiguous =
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                              R"("space":"display","path":")" +
                                  scene + R"("})");
        Check(!ambiguous.ok &&
                  ambiguous.message.find("wide, close") != std::string::npos,
              "two corrections and no id: the ids are listed: " +
                  ambiguous.message);
        Check(call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                                R"("space":"display","path":")" +
                                    scene + R"(","id":"close"})")
                      .ok &&
                  effectsOf(0).back().params.at("amount").num == -1 &&
                  effectsOf(0).back().params.at("amount").den == 10,
              "the one named applies");

        CheckFailureEnvelope(
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                              R"("space":"acescct","cdl":{"slope":"1 1 1",)"
                              R"("offset":"0 0 0","power":"1 1 1"}})"),
            "a CDL declared on ACEScct for a clip graded on display values",
            "ValidationFailed");
        CheckFailureEnvelope(
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                              R"("space":"rec709","cdl":{"slope":"1 1 1",)"
                              R"("offset":"0 0 0","power":"1 1 1"}})"),
            "an unknown space", "ValidationFailed");
        CheckFailureEnvelope(
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                              R"("space":"display"})"),
            "no CDL at all", "ValidationFailed");
        CheckFailureEnvelope(
            call("apply_cdl", R"({"clip_ids":["01K30000000000000000000003"],)"
                              R"("space":"display","cdl":{"slope":"1 1",)"
                              R"("offset":"0 0 0","power":"1 1 1"}})"),
            "a slope of two numbers", "ValidationFailed");
        std::filesystem::remove_all(directory);
    }

    // G2 -- match_color reads its two clips, positions, regions and aspects
    // exactly, and returns a step prepare_edit_plan takes as it is.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  "match_color arguments parse: " + parseError);
            return registry.Call(backend, "match_color", arguments);
        };
        const McpToolCallOutcome matched = call(
            R"({"clip_id":"01K30000000000000000000004",)"
            R"("reference_clip_id":"01K30000000000000000000003",)"
            R"("reference_position":"Start","match":["exposure","balance"],)"
            R"("reference_region":{"x":{"num":1,"den":4},"y":{"num":0,"den":1},)"
            R"("width":{"num":1,"den":2},"height":{"num":1,"den":1}}})");
        Check(matched.ok && backend.MatchedPlan().has_value(),
              "match_color reaches the backend: " + matched.message);
        if (backend.MatchedPlan()) {
            const ColorMatchPlan& plan = *backend.MatchedPlan();
            Check(plan.clip_id == "01K30000000000000000000004" &&
                      plan.reference_clip_id == "01K30000000000000000000003" &&
                      plan.position == "Middle" &&
                      plan.reference_position == "Start" &&
                      !plan.region.present && plan.reference_region.present &&
                      plan.reference_region.x.den == 4 &&
                      plan.aspects.exposure && plan.aspects.balance &&
                      !plan.aspects.saturation,
                  "the plan carries both clips, positions, regions and "
                  "aspects");
        }
        Check(matched.result_json.find(R"("tool":"set_clip_effects")") !=
                      std::string::npos &&
                  matched.result_json.find(
                      R"("clip_id":"01K30000000000000000000004")") !=
                      std::string::npos,
              "the reply carries a set_clip_effects step: " +
                  matched.result_json);
        Check(matched.result_json.find(R"("placement":"added")") !=
                  std::string::npos,
              "and where each stage went");

        CheckFailureEnvelope(
            call(R"({"clip_id":"01K30000000000000000000004"})"),
            "match_color without a reference", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_id":"01K30000000000000000000004",)"
                 R"("reference_clip_id":"01K30000000000000000000003",)"
                 R"("match":[]})"),
            "match_color matching nothing", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_id":"01K30000000000000000000004",)"
                 R"("reference_clip_id":"01K30000000000000000000003",)"
                 R"("match":["contrast"]})"),
            "match_color on an aspect it does not match", "ValidationFailed");
        CheckFailureEnvelope(
            call(R"({"clip_id":"01K30000000000000000000004",)"
                 R"("reference_clip_id":"01K30000000000000000000003",)"
                 R"("reference_position":"Later"})"),
            "match_color at an unknown position", "ValidationFailed");
    }

    // B8 -- timelines are project objects, not an array an MCP client edits.
    // Exercise the tools through a real ProjectEditLog, including the engine's
    // stable refusal for the final timeline.
    {
        InMemoryBackend timelineBackend(fixture, true);
        McpToolRegistry timelineRegistry;
        const McpToolCallOutcome listed = timelineRegistry.Call(
            timelineBackend, "list_timelines", mcp_json::Value::MakeObject());
        Check(
            listed.ok &&
                listed.result_json.find("\"width\":1920") !=
                    std::string::npos &&
                listed.result_json.find("\"active\":true") != std::string::npos,
            "list_timelines exposes dimensions and active state");
        Document liveDocument = fixture;
        EditLog liveLog;
        Project liveProject = Project::FromDocument(fixture);
        bool liveDescriptionUsed = false;
        McpLiveBackend liveBackend(liveDocument, liveLog, nullptr, nullptr,
                                   [&](std::string& json, std::string&) {
                                       liveDescriptionUsed = true;
                                       json = DescribeProject(liveProject);
                                       return true;
                                   });
        const McpToolCallOutcome liveListed = timelineRegistry.Call(
            liveBackend, "list_timelines", mcp_json::Value::MakeObject());
        Check(liveListed.ok && liveDescriptionUsed,
              "the live chat backend lists timelines from its project view");
        mcp_json::Value addArguments;
        std::string timelineParseError;
        Check(
            mcp_json::Value::Parse(
                R"({"name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1}})",
                addArguments, timelineParseError),
            "add_timeline arguments parse: " + timelineParseError);
        Check(
            timelineRegistry.Call(timelineBackend, "add_timeline", addArguments)
                .ok,
            "add_timeline applies an AddProjectTimelineOperation");
        const ProjectOperation* added = timelineBackend.LastProjectOperation();
        Check(added &&
                  std::holds_alternative<AddProjectTimelineOperation>(*added),
              "add_timeline uses the existing project operation");
        const Ulid addedId =
            std::get<AddProjectTimelineOperation>(*added).timeline_id;
        mcp_json::Value cloneArguments = mcp_json::Value::MakeObject();
        cloneArguments.Set("source_timeline_id",
                           mcp_json::Value::MakeString(addedId));
        cloneArguments.Set("name",
                           mcp_json::Value::MakeString("Event variant"));
        Check(timelineRegistry
                      .Call(timelineBackend, "clone_timeline", cloneArguments)
                      .ok &&
                  (timelineBackend.LastProjectOperation() != nullptr) &&
                  std::holds_alternative<CloneProjectTimelineOperation>(
                      *timelineBackend.LastProjectOperation()),
              "MCP cloning uses the reversible project operation");
        const Ulid clonedId = std::get<CloneProjectTimelineOperation>(
                                  *timelineBackend.LastProjectOperation())
                                  .timeline_id;
        mcp_json::Value removeClone = mcp_json::Value::MakeObject();
        removeClone.Set("timeline_id", mcp_json::Value::MakeString(clonedId));
        Check(timelineRegistry
                  .Call(timelineBackend, "remove_timeline", removeClone)
                  .ok,
              "remove the independent clone through its fresh ID");
        mcp_json::Value renameArguments;
        Check(mcp_json::Value::Parse("{\"timeline_id\":\"" +
                                         addedId.substr(0, 10) +
                                         "\",\"name\":\"Portrait\"}",
                                     renameArguments, timelineParseError),
              "rename_timeline arguments parse: " + timelineParseError);
        Check(timelineRegistry
                  .Call(timelineBackend, "rename_timeline", renameArguments)
                  .ok,
              "rename_timeline resolves an unambiguous ULID prefix");
        Check(timelineBackend.LastProjectOperation() &&
                  std::holds_alternative<RenameProjectItemOperation>(
                      *timelineBackend.LastProjectOperation()),
              "rename_timeline uses RenameProjectItemOperation");
        mcp_json::Value removeArguments;
        Check(mcp_json::Value::Parse("{\"timeline_id\":\"" + addedId + "\"}",
                                     removeArguments, timelineParseError),
              "remove_timeline arguments parse: " + timelineParseError);
        Check(timelineRegistry
                  .Call(timelineBackend, "remove_timeline", removeArguments)
                  .ok,
              "remove_timeline applies a RemoveProjectTimelineOperation");
        Check(timelineBackend.LastProjectOperation() &&
                  std::holds_alternative<RemoveProjectTimelineOperation>(
                      *timelineBackend.LastProjectOperation()),
              "remove_timeline uses the existing project operation");
        const McpToolCallOutcome remaining = timelineRegistry.Call(
            timelineBackend, "list_timelines", mcp_json::Value::MakeObject());
        mcp_json::Value remainingJson;
        Check(mcp_json::Value::Parse(remaining.result_json, remainingJson,
                                     timelineParseError),
              "remaining timeline list parses: " + timelineParseError);
        const mcp_json::Value* remainingTimelines =
            remainingJson.Find("timelines");
        const std::string remainingId =
            remainingTimelines && !remainingTimelines->AsArray().empty()
                ? remainingTimelines->AsArray().front().Find("id")->AsString()
                : "";
        Check(
            mcp_json::Value::Parse("{\"timeline_id\":\"" + remainingId + "\"}",
                                   removeArguments, timelineParseError),
            "final removal arguments parse: " + timelineParseError);
        const McpToolCallOutcome finalRemoval = timelineRegistry.Call(
            timelineBackend, "remove_timeline", removeArguments);
        CheckFailureEnvelope(finalRemoval, "remove final timeline",
                             "InvalidOperation");
        Check(mcp_json::Value::Parse(
                  R"({"timeline_id":"01ZZZZZZZZZZZZZZZZZZZZZZZZ"})",
                  removeArguments, timelineParseError),
              "unknown timeline arguments parse: " + timelineParseError);
        const McpToolCallOutcome unknownTimeline = timelineRegistry.Call(
            timelineBackend, "remove_timeline", removeArguments);
        CheckFailureEnvelope(unknownTimeline, "remove unknown timeline",
                             "UnknownSequence");
    }
    const McpToolCallOutcome transcriptOutcome = shortRegistry.Call(
        backend, "get_timeline_transcript", mcp_json::Value::MakeObject());
    Check(transcriptOutcome.ok && transcriptOutcome.result_json.find(
                                      "A strong hook") != std::string::npos,
          "get_timeline_transcript returns the backend's semantic spans");

    // B1 -- the tool surfaces both measured groups and the derived onset,
    // and analysis settings reach the cache-producing backend unchanged.
    {
        Document speechDocument = fixture;
        DocumentTrack audio = speechDocument.sequence.tracks.front();
        audio.id = "01K30000000000000000000006";
        audio.kind = "audio";
        audio.clips.resize(1);
        audio.clips[0].id = "01K30000000000000000000007";
        audio.clips[0].source_in = {0, 50};
        audio.clips[0].duration = {160, 50};
        audio.clips[0].timeline_in = {0, 25};
        speechDocument.sequence.tracks.push_back(audio);

        SpeechOnsetReport report;
        report.media_id = "01K30000000000000000000001";
        report.windows_per_second = 50;
        report.decode_sample_rate = 16000;
        report.levels.insert(report.levels.end(), 50, 90000);
        report.levels.insert(report.levels.end(), 10, 1000);
        report.levels.insert(report.levels.end(), 100, 280000);
        report.speech_level = SpeechLevelPercentile(report.levels, 90);
        report.noise_floor = SpeechLevelPercentile(report.levels, 5);

        InMemoryBackend speechBackend(speechDocument);
        speechBackend.SetSourceSpeechOnset(report);
        McpToolRegistry speechRegistry;
        const McpToolCallOutcome speechList = speechRegistry.Call(
            speechBackend, "list_speech_onsets", mcp_json::Value::MakeObject());
        Check(speechList.ok &&
                  speechList.result_json.find("\"groups\":[{") !=
                      std::string::npos &&
                  speechList.result_json.find(
                      "\"dominant_onset\":{\"value\":60,\"rate\":50}") !=
                      std::string::npos,
              "list_speech_onsets publishes groups and dominant_onset");

        mcp_json::Value analyzeArguments;
        std::string analyzeError;
        Check(
            mcp_json::Value::Parse(
                R"({"media_id":"01K30000000000000000000001","group_gap_ms":300,"group_floor_db":8})",
                analyzeArguments, analyzeError),
            "speech analysis arguments parse: " + analyzeError);
        Check(
            speechRegistry
                    .Call(speechBackend, "analyze_speech_onset",
                          analyzeArguments)
                    .ok &&
                speechBackend.speech_onset_analysis_seen &&
                speechBackend.speech_onset_settings.thresholds
                        .group_gap_milliseconds == 300 &&
                speechBackend.speech_onset_settings.thresholds.group_floor_db ==
                    8,
            "analyze_speech_onset forwards configurable group settings");

        // AGENT-2026-09 A8 -- a list of media is one call: one line per
        // media, the settings forwarded to each; one media or a list, never
        // both, never neither.
        mcp_json::Value batchArguments;
        Check(mcp_json::Value::Parse(
                  R"({"media_ids":["01K30000000000000000000001",)"
                  R"("01K30000000000000000000001"],"group_gap_ms":250})",
                  batchArguments, analyzeError),
              "batch analysis arguments parse: " + analyzeError);
        const McpToolCallOutcome batch = speechRegistry.Call(
            speechBackend, "analyze_speech_onset", batchArguments);
        mcp_json::Value batchView;
        Check(batch.ok &&
                  mcp_json::Value::Parse(batch.result_json, batchView,
                                         analyzeError) &&
                  batchView.Find("analyzed") &&
                  batchView.Find("analyzed")->Dump() == "2" &&
                  batchView.Find("failed")->Dump() == "0" &&
                  batchView.Find("results")->AsArray().size() == 2 &&
                  batchView.Find("results")->AsArray()[1].Find("ok") &&
                  batchView.Find("results")->AsArray()[1].Find("result") &&
                  speechBackend.speech_onset_settings.thresholds
                          .group_gap_milliseconds == 250,
              "analyze_speech_onset takes a list and answers one line per "
              "media: " +
                  batch.result_json);
        CheckFailureEnvelope(
            speechRegistry.Call(speechBackend, "analyze_speech_onset",
                                mcp_json::Value::MakeObject()),
            "an analysis with neither media_id nor media_ids",
            "ValidationFailed");
        mcp_json::Value both;
        mcp_json::Value::Parse(R"({"media_id":"01K30000000000000000000001",)"
                               R"("media_ids":["01K30000000000000000000001"]})",
                               both, analyzeError);
        CheckFailureEnvelope(
            speechRegistry.Call(speechBackend, "analyze_speech_onset", both),
            "an analysis with both media_id and media_ids", "ValidationFailed");
    }

    // AGENT-2026-09 A4 -- the words of a rush with where to cut on each,
    // through the tool: the envelope decides the frames, `find` narrows
    // to a word and its neighbours, max_words caps and says how to go on,
    // and without an envelope every rule says so.
    {
        Document wordsDocument = fixture;
        InMemoryBackend wordsBackend(wordsDocument);
        Transcript transcript;
        transcript.media_id = "01K30000000000000000000001";
        transcript.source_rate = {25, 1};
        transcript.word_timing = "nemo";
        transcript.verbatim = true;
        const std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>>
            spoken = {{"Alors", {10, 20}},
                      {"le", {22, 25}},
                      {"Projet", {26, 40}},
                      {"avance", {41, 50}}};
        for (const auto& [text, frames] : spoken) {
            TranscriptWord word;
            word.text = text;
            word.start = {frames.first, 25};
            word.end = {frames.second, 25};
            transcript.words.push_back(word);
        }
        wordsBackend.SetSourceTranscript(transcript);
        // Two windows per frame; voice from frame 12 to 48, so "Alors"
        // starts in silence (attack at 12) and "avance" ends in silence
        // (last voice at 47).
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000001";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.speech_level = 1000;
        envelope.noise_floor = 10;
        envelope.levels.assign(120, 10);
        for (size_t window = 24; window < 96; ++window)
            envelope.levels[window] = 1000;
        McpToolRegistry wordsRegistry;
        mcp_json::Value wordsArguments;
        std::string wordsError;
        const auto callWords = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  "list_source_words arguments parse: " + parseError);
            return wordsRegistry.Call(wordsBackend, "list_source_words",
                                      arguments);
        };
        McpToolCallOutcome noEnvelope =
            callWords(R"({"media_id":"01K30000000000000000000001"})");
        mcp_json::Value view;
        Check(
            noEnvelope.ok &&
                mcp_json::Value::Parse(noEnvelope.result_json, view,
                                       wordsError) &&
                view.Find("envelope")->AsString() == "missing" &&
                view.Find("words")->AsArray().size() == 4 &&
                view.Find("words")->AsArray()[0].Find("entry_rule") &&
                view.Find("words")
                        ->AsArray()[0]
                        .Find("entry_rule")
                        ->AsString() == "no_envelope" &&
                view.Find("words")->AsArray()[0].Find("entry_frame")->Dump() ==
                    "10" &&
                view.Find("word_timing")->AsString() == "nemo",
            "without an envelope the words keep the aligner's frames and "
            "say so: " +
                noEnvelope.result_json);
        wordsBackend.SetSourceSpeechOnset(envelope);
        const McpToolCallOutcome all =
            callWords(R"({"media_id":"01K30000000000000000000001"})");
        Check(all.ok &&
                  mcp_json::Value::Parse(all.result_json, view, wordsError) &&
                  view.Find("envelope")->AsString() == "cached" &&
                  view.Find("threshold_dbfs") != nullptr &&
                  view.Find("total_words")->Dump() == "4" &&
                  view.Find("truncated")->AsBool() == false &&
                  // AGENT-2026-09 A18 -- every voiced frame here belongs to
                  // a word: the scan is present and empty.
                  view.Find("voice_without_words") != nullptr &&
                  view.Find("voice_without_words")->AsArray().empty(),
              "with an envelope the answer names it and scans for voice "
              "without words: " +
                  all.result_json);
        if (all.ok && view.Find("words") &&
            view.Find("words")->AsArray().size() == 4) {
            const auto& words = view.Find("words")->AsArray();
            Check(words[0].Find("entry_frame")->Dump() == "10" &&
                      words[0].Find("entry_rule")->AsString() == "attack_ahead",
                  "a start in silence takes the attack less two frames: " +
                      words[0].Dump());
            Check(words[0].Find("exit_frame")->Dump() == "22" &&
                      words[0].Find("exit_rule")->AsString() == "next_word" &&
                      words[0].Find("gap_after_frames")->Dump() == "2",
                  "a voiced end without a drop stops at the next word: " +
                      words[0].Dump());
            Check(
                words[3].Find("exit_frame")->Dump() == "50" &&
                    words[3].Find("exit_rule")->AsString() == "voice_before" &&
                    words[3].Find("gap_after_frames") == nullptr,
                "an end in silence pulls back to the voice plus three "
                "frames, capped at the aligner's end: " +
                    words[3].Dump());
        }
        const McpToolCallOutcome found = callWords(
            R"({"media_id":"01K30000000000000000000001","find":"PROJET",)"
            R"("context_words":1})");
        Check(found.ok &&
                  mcp_json::Value::Parse(found.result_json, view, wordsError) &&
                  view.Find("matches")->Dump() == "1" &&
                  view.Find("words")->AsArray().size() == 3 &&
                  view.Find("words")->AsArray()[1].Find("text")->AsString() ==
                      "Projet",
              "find is case folded and brings the neighbours: " +
                  found.result_json);
        const McpToolCallOutcome capped = callWords(
            R"({"media_id":"01K30000000000000000000001","max_words":2})");
        Check(
            capped.ok &&
                mcp_json::Value::Parse(capped.result_json, view, wordsError) &&
                view.Find("words")->AsArray().size() == 2 &&
                view.Find("truncated")->AsBool() &&
                view.Find("next_from_frame")->Dump() == "26",
            "max_words caps the answer and says where to continue: " +
                capped.result_json);
        const McpToolCallOutcome ranged = callWords(
            R"({"media_id":"01K30000000000000000000001","from_frame":26})");
        Check(
            ranged.ok &&
                mcp_json::Value::Parse(ranged.result_json, view, wordsError) &&
                view.Find("words")->AsArray().size() == 2 &&
                view.Find("words")->AsArray()[0].Find("text")->AsString() ==
                    "Projet",
            "from_frame leaves out the words that end before it: " +
                ranged.result_json);
        CheckFailureEnvelope(
            callWords(R"({"media_id":"01K30000000000000000000001",)"
                      R"("from_frame":30,"to_frame":20})"),
            "a range that ends before it starts", "ValidationFailed");
        CheckFailureEnvelope(
            callWords(R"({"media_id":"01K30000000000000000000001",)"
                      R"("max_words":0})"),
            "a zero cap", "ValidationFailed");
        // AGENT-2026-09 A14 -- the envelope where the rules decided: one
        // dBFS per frame, three frames before the entry to three after the
        // exit. "Alors" enters at 10 and exits at 22: frames 7 to 24.
        const McpToolCallOutcome levelled = callWords(
            R"({"media_id":"01K30000000000000000000001","levels":true,)"
            R"("max_words":1})");
        Check(levelled.ok &&
                  mcp_json::Value::Parse(levelled.result_json, view,
                                         wordsError) &&
                  view.Find("words")->AsArray().size() == 1 &&
                  view.Find("words")->AsArray()[0].Find("levels_from_frame") &&
                  view.Find("words")
                          ->AsArray()[0]
                          .Find("levels_from_frame")
                          ->Dump() == "7" &&
                  view.Find("words")->AsArray()[0].Find("levels_dbfs") &&
                  view.Find("words")
                          ->AsArray()[0]
                          .Find("levels_dbfs")
                          ->AsArray()
                          .size() == 18,
              "levels:true gives the envelope around the word's entry and "
              "exit: " +
                  levelled.result_json);
        // AGENT-2026-09 A23 -- with a range, the whole envelope of it.
        const McpToolCallOutcome rangeLevels = callWords(
            R"({"media_id":"01K30000000000000000000001","levels":true,)"
            R"("from_frame":8,"to_frame":30})");
        Check(
            rangeLevels.ok &&
                mcp_json::Value::Parse(rangeLevels.result_json, view,
                                       wordsError) &&
                view.Find("range_levels") != nullptr &&
                view.Find("range_levels")->Find("from_frame")->Dump() == "8" &&
                view.Find("range_levels")->Find("end_frame")->Dump() == "30" &&
                view.Find("range_levels")
                        ->Find("levels_dbfs")
                        ->AsArray()
                        .size() == 22 &&
                view.Find("range_levels")->Find("speech_level_dbfs") &&
                view.Find("range_levels")->Find("floor_dbfs") &&
                view.Find("range_levels")->Find("truncated") == nullptr,
            "levels with a range gives one dBFS per frame over it: " +
                rangeLevels.result_json);

        // AGENT-2026-09 A24 -- the montage's words from the alignment: the
        // audio clip reads frames 20..30 of the rush, so it holds "le" whole
        // and the head of "Projet", cut at its out.
        Document montage = fixture;
        montage.sequence.tracks[1].clips[0].source_in = {20, 25};
        InMemoryBackend montageBackend(montage);
        montageBackend.SetSourceTranscript(transcript);
        montageBackend.SetSourceSpeechOnset(envelope);
        const auto callMontage = [&](const std::string& json) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, arguments, parseError),
                  "list_timeline_words arguments parse: " + parseError);
            return wordsRegistry.Call(montageBackend, "list_timeline_words",
                                      arguments);
        };
        const McpToolCallOutcome montageWords = callMontage("{}");
        Check(montageWords.ok &&
                  mcp_json::Value::Parse(montageWords.result_json, view,
                                         wordsError) &&
                  view.Find("text")->AsString() == "le Projet" &&
                  view.Find("clips")->AsArray().size() == 1 &&
                  view.Find("summary")->Find("cut_into_words")->Dump() == "1" &&
                  view.Find("summary")->Find("checked")->Dump() == "1",
              "the montage's words come from the alignment, clip by clip: " +
                  montageWords.result_json);
        if (montageWords.ok && view.Find("clips") &&
            view.Find("clips")->AsArray().size() == 1) {
            const mcp_json::Value& clip = view.Find("clips")->AsArray()[0];
            Check(
                clip.Find("clip_id")->AsString() ==
                        "01K30000000000000000000006" &&
                    clip.Find("source_in_frame")->Dump() == "20" &&
                    clip.Find("source_end_frame")->Dump() == "30" &&
                    clip.Find("timeline_in_frame")->Dump() == "5" &&
                    clip.Find("words")->AsArray().size() == 2 &&
                    clip.Find("words")->AsArray()[0].Find("whole") == nullptr &&
                    clip.Find("words")
                            ->AsArray()[0]
                            .Find("timeline_frame")
                            ->Dump() == "7" &&
                    clip.Find("words")->AsArray()[1].Find("whole")->Dump() ==
                        "false" &&
                    !clip.Find("head_cut")->AsBool() &&
                    clip.Find("tail_cut")->AsBool() &&
                    clip.Find("air")->Find("head_frames")->Dump() == "0" &&
                    clip.Find("voice_without_words")->AsArray().empty(),
                "a clip names its words, where they play, and the word "
                "its out cuts into: " +
                    clip.Dump());
        }
        const McpToolCallOutcome montageText =
            callMontage(R"({"text_only":true})");
        Check(montageText.ok &&
                  mcp_json::Value::Parse(montageText.result_json, view,
                                         wordsError) &&
                  view.Find("clips") == nullptr &&
                  view.Find("text")->AsString() == "le Projet",
              "text_only leaves out the per-clip detail: " +
                  montageText.result_json);
        CheckFailureEnvelope(callMontage(R"({"clip_ids":"x"})"),
                             "clip_ids that is not an array",
                             "ValidationFailed");
    }
    // Segments are named by span id and resolved by the engine. A caller
    // that could state a source_in could state one landing mid-word, and
    // ApplyOperation would accept it: it only checks that a range sits
    // inside the media. Refusing the field is what makes that unbuildable.
    //
    // IA3 -- ids come with the revision of the reading they were taken from,
    // read here the way an agent reads it: from get_timeline_transcript.
    const auto currentRevision = [&]() {
        const McpToolCallOutcome read = shortRegistry.Call(
            backend, "get_timeline_transcript", mcp_json::Value::MakeObject());
        mcp_json::Value view;
        std::string parseFailure;
        const mcp_json::Value* revision =
            read.ok &&
                    mcp_json::Value::Parse(read.result_json, view, parseFailure)
                ? view.Find("transcript_revision")
                : nullptr;
        return revision != nullptr && revision->IsString()
                   ? revision->AsString()
                   : std::string();
    };
    const auto callTool = [&](const std::string& tool,
                              const std::string& argumentsJson,
                              bool withRevision) {
        mcp_json::Value arguments;
        std::string parseFailure;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
              "short tool arguments parse: " + parseFailure);
        if (withRevision && arguments.Find("transcript_revision") == nullptr) {
            arguments.Set("transcript_revision",
                          mcp_json::Value::MakeString(currentRevision()));
        }
        return shortRegistry.Call(backend, tool, arguments);
    };
    const auto callShort = [&](const std::string& argumentsJson) {
        return callTool("create_interview_short", argumentsJson, true);
    };
    const auto capturedSegments = [&]() {
        const ProjectOperation* captured = backend.LastProjectOperation();
        return captured && std::holds_alternative<
                               CreateProjectTimelineFromSegmentsOperation>(
                               *captured)
                   ? std::get<CreateProjectTimelineFromSegmentsOperation>(
                         *captured)
                         .segments
                   : std::vector<ProjectTimelineSourceSegment>{};
    };

    Check(
        callShort(R"({"name":"Short test","segments":[{"span_id":"S1"}]})").ok,
        "create_interview_short accepts a span id");
    Check(capturedSegments().size() == 1 &&
              capturedSegments()[0].source_in == RationalTime{100, 25} &&
              capturedSegments()[0].duration == RationalTime{10, 25} &&
              capturedSegments()[0].source_id == "01K30000000000000000000001",
          "a span id resolves to that span's exact source range");

    Check(callShort(R"({"segments":[{"span_id":"S1","end_span_id":"S2"}]})").ok,
          "create_interview_short accepts a contiguous span run");
    Check(capturedSegments().size() == 1 &&
              capturedSegments()[0].source_in == RationalTime{100, 25} &&
              capturedSegments()[0].duration == RationalTime{25, 25},
          "a contiguous run merges into one exact range, computed by the "
          "engine rather than added up by the caller");

    // SYNCHRO-2026-10 SY3 -- a rush synced to a recorder keeps its sound:
    // the tool reads the link in the timeline the span was read in, and
    // finds the recorder's range at the same instants (picture 100/25 at
    // 5/25, recorder 300/25 at 5/25: 200 frames ahead).
    {
        Document synced = Fixture();
        synced.sources.push_back({"01K30000000000000000000007",
                                  "folder/A.WAV",
                                  {25, 1},
                                  {1000, 25}});
        DocumentClip heard{"01K30000000000000000000009",
                           "01K30000000000000000000007",
                           {300, 25},
                           {10, 25},
                           {5, 25}};
        heard.link_group_id = "01K30000000000000000000005";
        synced.sequence.tracks.push_back(
            {"01K30000000000000000000008", "audio", 2, {heard}});
        InMemoryBackend syncedBackend(synced);
        McpToolRegistry syncedRegistry;
        const McpToolCallOutcome read =
            syncedRegistry.Call(syncedBackend, "get_timeline_transcript",
                                mcp_json::Value::MakeObject());
        mcp_json::Value view;
        std::string parseFailure;
        const mcp_json::Value* revision =
            read.ok &&
                    mcp_json::Value::Parse(read.result_json, view, parseFailure)
                ? view.Find("transcript_revision")
                : nullptr;
        mcp_json::Value arguments;
        Check(mcp_json::Value::Parse(R"({"segments":[{"span_id":"S1"}]})",
                                     arguments, parseFailure) &&
                  revision != nullptr && revision->IsString(),
              "the synced timeline is read: " + parseFailure);
        if (revision != nullptr && revision->IsString()) {
            arguments.Set("transcript_revision", *revision);
        }
        const McpToolCallOutcome made = syncedRegistry.Call(
            syncedBackend, "create_interview_short", arguments);
        const ProjectOperation* captured = syncedBackend.LastProjectOperation();
        const bool carried =
            made.ok && captured != nullptr &&
            std::holds_alternative<CreateProjectTimelineFromSegmentsOperation>(
                *captured) &&
            std::get<CreateProjectTimelineFromSegmentsOperation>(*captured)
                    .segments.size() == 1;
        const ProjectTimelineSourceSegment segment =
            carried ? std::get<CreateProjectTimelineFromSegmentsOperation>(
                          *captured)
                          .segments[0]
                    : ProjectTimelineSourceSegment{};
        Check(carried && segment.source_id == "01K30000000000000000000001" &&
                  segment.source_in == RationalTime{100, 25} &&
                  segment.separate_source_id == "01K30000000000000000000007" &&
                  segment.separate_source_in == RationalTime{300, 25},
              "create_interview_short carries the recorder's sound linked to "
              "the picture, at the same instants: " +
                  made.message);
    }

    const McpToolCallOutcome gapOutcome =
        callShort(R"({"segments":[{"span_id":"S1","end_span_id":"S3"}]})");
    Check(!gapOutcome.ok &&
              gapOutcome.message.find("breath") != std::string::npos,
          "a run spanning a real silence is refused, not stitched over");
    CheckFailureEnvelope(gapOutcome, "non-contiguous interview span",
                         "ValidationFailed");

    Check(!callShort(R"({"segments":[{"span_id":"S9"}]})").ok,
          "an unknown span id is refused");

    backend.SetTimelineTranscriptHallucinated(true);
    const McpToolCallOutcome unsafeShort =
        callShort(R"({"segments":[{"span_id":"S1"}]})");
    CheckFailureEnvelope(unsafeShort, "hallucinated interview transcript",
                         "ValidationFailed");
    Check(unsafeShort.message.find("force:true") != std::string::npos,
          "the interview refusal names the explicit override");
    Check(callShort(R"({"segments":[{"span_id":"S1"}],"force":true})").ok,
          "create_interview_short accepts an explicit hallucination "
          "override");
    backend.SetTimelineTranscriptHallucinated(false);

    // IA3 -- docs/roadmap/IA-2026-09.md. A selection without its reading, or
    // from a reading the view has moved past, is refused before anything is
    // resolved.
    {
        const std::string revision = currentRevision();
        Check(revision.size() == 16,
              "get_timeline_transcript returns its transcript_revision");
        const McpToolCallOutcome missing =
            callTool("create_interview_short",
                     R"({"segments":[{"span_id":"S1"}]})", false);
        CheckFailureEnvelope(missing, "selection without revision",
                             "ValidationFailed");
        Check(missing.message.find("transcript_revision") != std::string::npos,
              "the refusal names the missing revision");

        const ProjectOperation* lastBefore = backend.LastProjectOperation();
        const ProjectOperation keptOperation =
            lastBefore != nullptr ? *lastBefore : ProjectOperation{};
        // Any change to the view moves the revision; the hallucination
        // marker is the one this backend can flip.
        backend.SetTimelineTranscriptHallucinated(true);
        const McpToolCallOutcome stale =
            callTool("create_interview_short",
                     R"({"segments":[{"span_id":"S1"}],"force":true,)"
                     R"("transcript_revision":")" +
                         revision + R"("})",
                     false);
        backend.SetTimelineTranscriptHallucinated(false);
        CheckFailureEnvelope(stale, "stale selection", "StaleSelection");
        Check(stale.message.find(revision) != std::string::npos,
              "the stale refusal names the revision it was given");
        Check(backend.LastProjectOperation() != nullptr &&
                  SerializeProjectOperation(*backend.LastProjectOperation()) ==
                      SerializeProjectOperation(keptOperation),
              "a stale selection builds nothing");
        Check(callTool("create_interview_short",
                       R"({"segments":[{"span_id":"S1"}],)"
                       R"("transcript_revision":")" +
                           revision + R"("})",
                       false)
                  .ok,
              "the same revision is current again once the view is back");

        const McpToolCallOutcome blocks =
            callTool("list_speech_blocks", "{}", false);
        Check(blocks.ok &&
                  blocks.result_json.find("\"transcript_revision\":\"" +
                                          revision + "\"") != std::string::npos,
              "list_speech_blocks names the same revision: " +
                  blocks.result_json);
        Check(blocks.result_json.find("\"block_id\":\"B3\"") !=
                      std::string::npos &&
                  blocks.result_json.find("\"block_id\":\"B4\"") ==
                      std::string::npos,
              "three sentences are three blocks");

        Check(
            callShort(R"({"segments":[{"block_id":"B1","end_block_id":"B2"}]})")
                .ok,
            "create_interview_short takes a block run");
        Check(capturedSegments().size() == 1 &&
                  capturedSegments()[0].source_in == RationalTime{100, 25} &&
                  capturedSegments()[0].duration == RationalTime{25, 25},
              "B1 to B2 is S1 to S2, exactly");
        CheckFailureEnvelope(
            callShort(
                R"({"segments":[{"block_id":"B1","end_block_id":"B3"}]})"),
            "block run over a silence", "ValidationFailed");

        const McpToolCallOutcome inspected =
            callTool("inspect_speech_selection",
                     R"({"segments":[{"block_id":"B2"}]})", true);
        Check(inspected.ok &&
                  inspected.result_json.find(
                      "\"before\":{\"block_id\":\"B1\",\"span_id\":"
                      "\"S1\",\"text\":\"A strong hook.\"") !=
                      std::string::npos &&
                  inspected.result_json.find("\"kept\":false") !=
                      std::string::npos,
              "inspect_speech_selection shows what B2 leaves out before it: " +
                  inspected.result_json);
        CheckFailureEnvelope(
            callTool("inspect_speech_selection",
                     R"({"segments":[{"block_id":"B2"}],)"
                     R"("transcript_revision":"0000000000000000"})",
                     false),
            "stale inspection", "StaleSelection");

        const McpToolCallOutcome anchorMissing =
            callTool("place_motion_clip",
                     R"({"composition_id":"x","span_id":"S1"})", false);
        CheckFailureEnvelope(anchorMissing, "span anchor without revision",
                             "ValidationFailed");
        Check(anchorMissing.message.find("transcript_revision") !=
                  std::string::npos,
              "place_motion_clip asks for the revision with span_id");
        CheckFailureEnvelope(
            callTool("place_motion_clip",
                     R"({"composition_id":"x","span_id":"S1",)"
                     R"("transcript_revision":"0000000000000000"})",
                     false),
            "stale span anchor", "StaleSelection");
    }

    // The regression this whole shape exists to prevent.
    Check(
        !callShort(
             R"({"segments":[{"source_id":"01K30000000000000000000001","source_in":{"value":103,"rate":25},"duration":{"value":10,"rate":25}}]})")
             .ok,
        "a raw source range is refused: the model never states a time");

    // B12 -- a transcript whose words overlap no measured speech group is
    // visible for diagnosis but cannot drive word cuts without an explicit
    // override. Both the read-only proposal and the mutating tool enforce the
    // same cache marker.
    {
        Transcript unsafeTranscript;
        unsafeTranscript.media_id = "01K30000000000000000000001";
        unsafeTranscript.whisper_model = "large-v3";
        unsafeTranscript.source_rate = {25, 1};
        unsafeTranscript.speech_assessed = true;
        unsafeTranscript.measured_speech_duration = {0, 50};
        unsafeTranscript.likely_hallucinated = true;
        unsafeTranscript.known_hallucination_phrase = true;
        unsafeTranscript.words = {
            {"Sous-titrage", {100, 25}, {102, 25}},
            {"Société", {102, 25}, {104, 25}},
        };
        InMemoryBackend guardedBackend(fixture);
        guardedBackend.SetSourceTranscript(unsafeTranscript);
        McpToolRegistry guardedRegistry;
        mcp_json::Value listArguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003"})", listArguments,
                  parseError),
              "guarded list arguments parse: " + parseError);
        const McpToolCallOutcome refusedList = guardedRegistry.Call(
            guardedBackend, "list_disfluencies", listArguments);
        CheckFailureEnvelope(refusedList, "hallucinated disfluency list",
                             "ValidationFailed");
        Check(refusedList.message.find("force:true") != std::string::npos,
              "the disfluency refusal names the explicit override");
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003","force":true})",
                  listArguments, parseError) &&
                  guardedRegistry
                      .Call(guardedBackend, "list_disfluencies", listArguments)
                      .ok,
              "list_disfluencies accepts force:true");

        mcp_json::Value removeArguments;
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","ranges":[{"start_word_index":0,"end_word_index":0}]})",
                removeArguments, parseError),
            "guarded removal arguments parse: " + parseError);
        const McpToolCallOutcome refusedRemoval = guardedRegistry.Call(
            guardedBackend, "remove_words", removeArguments);
        CheckFailureEnvelope(refusedRemoval, "hallucinated word removal",
                             "ValidationFailed");
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","ranges":[{"start_word_index":0,"end_word_index":0}],"force":true})",
                removeArguments, parseError) &&
                guardedRegistry
                    .Call(guardedBackend, "remove_words", removeArguments)
                    .ok,
            "remove_words accepts force:true");
    }

    // ---- clean_disfluencies: one intention, one reversible operation ----
    // The clip covers source frames [100, 110) at 25/s, so every word below
    // sits inside it and the "euh" is the only filler.
    Transcript transcript;
    transcript.media_id = "01K30000000000000000000001";
    transcript.whisper_model = "large-v3";
    transcript.verbatim = true;
    transcript.source_rate = {25, 1};
    transcript.words = {
        {"Le", {100, 25}, {102, 25}},
        {"euh", {102, 25}, {104, 25}},
        {"montage", {104, 25}, {107, 25}},
        {"avance", {107, 25}, {110, 25}},
    };

    InMemoryBackend cleanBackend(fixture);
    cleanBackend.SetSourceTranscript(transcript);
    McpToolRegistry cleanRegistry;
    const std::string beforeClean =
        cleanBackend.CurrentDocument().SaveToString();
    mcp_json::Value cleanArguments;
    std::string cleanParseError;
    Check(mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000003"})",
                                 cleanArguments, cleanParseError),
          "clean_disfluencies arguments parse: " + cleanParseError);
    const McpToolCallOutcome cleanOutcome =
        cleanRegistry.Call(cleanBackend, "clean_disfluencies", cleanArguments);
    Check(cleanOutcome.ok,
          "clean_disfluencies applies: " + cleanOutcome.message);
    Check(cleanOutcome.result_json.find("\"removed_count\":1") !=
                  std::string::npos &&
              cleanOutcome.result_json.find("euh") != std::string::npos,
          "clean_disfluencies reports the filler it cut, by text");
    Check(
        cleanOutcome.result_json.find("\"verbatim\":true") != std::string::npos,
        "clean_disfluencies reports which decoding the transcript came "
        "from, because a standard one would say nothing about the take");
    Check(cleanBackend.CurrentDocument().SaveToString() != beforeClean,
          "clean_disfluencies actually changed the document");

    // One tool call is one operation, so one undo restores the document
    // byte for byte -- the property that makes this safe to point at an
    // agent in the first place.
    std::string cleanUndoErrorName;
    std::string cleanUndoMessage;
    std::string cleanUndoResult;
    Check(cleanBackend.Undo(cleanUndoResult, cleanUndoErrorName,
                            cleanUndoMessage),
          "the cleanup undoes: " + cleanUndoMessage);
    Check(cleanBackend.CurrentDocument().SaveToString() == beforeClean,
          "undoing one clean_disfluencies call restores the exact prior "
          "bytes, so the whole cleanup is a single reversible step");

    // Cleaning a clip with nothing to cut is an honoured request, not an
    // error, and must not touch the document.
    Transcript clean;
    clean.media_id = "01K30000000000000000000001";
    clean.whisper_model = "large-v3";
    clean.verbatim = true;
    clean.source_rate = {25, 1};
    clean.words = {{"Le", {100, 25}, {104, 25}},
                   {"montage", {104, 25}, {110, 25}}};
    InMemoryBackend tidyBackend(fixture);
    tidyBackend.SetSourceTranscript(clean);
    const std::string beforeTidy = tidyBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome tidyOutcome =
        cleanRegistry.Call(tidyBackend, "clean_disfluencies", cleanArguments);
    Check(tidyOutcome.ok && tidyOutcome.result_json.find("\"applied\":false") !=
                                std::string::npos,
          "cleaning an already clean clip succeeds without applying anything");
    Check(tidyBackend.CurrentDocument().SaveToString() == beforeTidy,
          "a no-op cleanup leaves the document untouched");

    // An A/V pair is resolved by the tool, not by the caller: naming the
    // audio clip must take its linked picture with it, in one operation.
    Document avFixture;
    avFixture.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}}};
    const auto linkedTrack = [](const Ulid& trackId, const std::string& kind,
                                int32_t index, const Ulid& clipId) {
        DocumentTrack track;
        track.id = trackId;
        track.kind = kind;
        track.index = index;
        DocumentClip clip{
            clipId, "01K30000000000000000000001", {100, 25}, {10, 25}, {0, 25}};
        clip.link_group_id = "01K30000000000000000000050";
        track.clips = {clip};
        return track;
    };
    avFixture.sequence.tracks = {
        linkedTrack("01K30000000000000000000051", "video", 0,
                    "01K30000000000000000000052"),
        linkedTrack("01K30000000000000000000053", "audio", 1,
                    "01K30000000000000000000054"),
    };

    InMemoryBackend avBackend(avFixture);
    avBackend.SetSourceTranscript(transcript);
    mcp_json::Value avArguments;
    std::string avParseError;
    Check(mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000054"})",
                                 avArguments, avParseError),
          "A/V cleanup arguments parse: " + avParseError);
    const std::string beforeAv = avBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome avOutcome =
        cleanRegistry.Call(avBackend, "clean_disfluencies", avArguments);
    Check(avOutcome.ok,
          "clean_disfluencies applies to a pair: " + avOutcome.message);
    Check(avOutcome.result_json.find("01K30000000000000000000052") !=
              std::string::npos,
          "the result names the linked picture it also cut, so the caller "
          "can see it happened");
    const DocumentTrack& avVideo =
        avBackend.CurrentDocument().sequence.tracks[0];
    const DocumentTrack& avAudio =
        avBackend.CurrentDocument().sequence.tracks[1];
    Check(avVideo.clips.size() == avAudio.clips.size(),
          "picture and sound end up with the same number of fragments");
    bool alignedPair = avVideo.clips.size() == avAudio.clips.size();
    for (size_t index = 0; index < avVideo.clips.size() && alignedPair; ++index)
        alignedPair =
            avVideo.clips[index].timeline_in ==
                avAudio.clips[index].timeline_in &&
            avVideo.clips[index].duration == avAudio.clips[index].duration;
    Check(alignedPair,
          "and they stay frame-aligned: cleaning the sound did not slide it "
          "out from under the picture");

    std::string avUndoResult;
    std::string avUndoErrorName;
    std::string avUndoMessage;
    Check(avBackend.Undo(avUndoResult, avUndoErrorName, avUndoMessage) &&
              avBackend.CurrentDocument().SaveToString() == beforeAv,
          "one undo restores both tracks: the pair cut is a single step");

    // ITW2 -- hesitations from the signal join the transcribed ones, with a
    // cut in the rush's frames that remove_source_ranges applies to the pair.
    {
        InMemoryBackend hesitationBackend(avFixture);
        hesitationBackend.SetSourceTranscript(transcript);
        mcp_json::Value listArguments;
        std::string listParseError;
        mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000054"})",
                               listArguments, listParseError);
        McpToolCallOutcome listed = cleanRegistry.Call(
            hesitationBackend, "list_disfluencies", listArguments);
        Check(listed.ok && listed.result_json.find(
                               "\"hesitation_analysis\":\"missing\"") !=
                               std::string::npos,
              "without an analysis the view says so: " + listed.result_json);
        HesitationReport hesitations;
        hesitations.media_id = "01K30000000000000000000001";
        hesitations.median_stability_milli = 33000;
        HesitationCandidate stretched;
        stretched.family = "stretched_syllable";
        stretched.start = {408, 100};  // source frame 102
        stretched.end = {424, 100};    // frame 106, the word going on
        stretched.stability_milli = 22000;
        stretched.within_word = "l'Horloge";
        HesitationCandidate outside = stretched;
        outside.family = "filler";
        outside.start = {900, 100};  // beyond the clip
        outside.end = {960, 100};
        hesitations.candidates = {stretched, outside};
        hesitationBackend.SetSourceHesitations(hesitations);
        listed = cleanRegistry.Call(hesitationBackend, "list_disfluencies",
                                    listArguments);
        mcp_json::Value listView;
        Check(listed.ok && mcp_json::Value::Parse(listed.result_json, listView,
                                                  listParseError),
              "list_disfluencies with hesitations parses");
        const mcp_json::Value* found = listView.Find("hesitations");
        int64_t first = -1;
        int64_t end = -1;
        int64_t cutFirst = -1;
        int64_t percent = -1;
        if (found != nullptr && found->IsArray() &&
            found->AsArray().size() == 1) {
            const mcp_json::Value& entry = found->AsArray()[0];
            entry.Find("first_frame")->AsInt64(first);
            entry.Find("end_frame")->AsInt64(end);
            entry.Find("suggested_cut")->Find("first_frame")->AsInt64(cutFirst);
            entry.Find("stability_percent_of_median")->AsInt64(percent);
        }
        // AGENT-2026-09 A47 -- two frames of the syllable's head are kept.
        Check(first == 102 && end == 106 && cutFirst == 104 && percent == 66,
              "only the hesitation inside the clip is listed, in source "
              "frames, keeping the head of a stretched syllable: " +
                  listed.result_json);

        // AGENT-2026-09 A33 bis, A43 -- a filler the aligner buried in the
        // previous word's tail keeps its cut, the next word's attack is
        // never taken, and a long aligned word with a hollow between two
        // stretches of voice is a restart whose cut removes the first
        // utterance whole.
        {
            Document restartDocument;
            restartDocument.sources = {{"01K30000000000000000000001",
                                        "folder/A.MP4",
                                        {25, 1},
                                        {1000, 25}}};
            // A second clip entered at 46, on the decay of the hold placed
            // at 40..45 below (A48).
            restartDocument.sequence.tracks = {{"01K30000000000000000000082",
                                                "video",
                                                0,
                                                {{"01K30000000000000000000083",
                                                  "01K30000000000000000000001",
                                                  {0, 25},
                                                  {80, 25},
                                                  {0, 25}},
                                                 {"01K30000000000000000000084",
                                                  "01K30000000000000000000001",
                                                  {46, 25},
                                                  {34, 25},
                                                  {80, 25}}}}};
            Transcript aligned;
            aligned.media_id = "01K30000000000000000000001";
            aligned.source_rate = {25, 1};
            aligned.word_timing = "nemo";
            aligned.verbatim = true;
            const std::vector<
                std::pair<std::string, std::pair<int64_t, int64_t>>>
                spoken = {{"sur", {5, 7}},
                          {"horloge", {10, 40}},
                          {"du", {41, 44}},
                          {"souvent", {50, 60}},
                          {"dormir", {61, 70}}};
            for (const auto& [text, frames] : spoken) {
                TranscriptWord word;
                word.text = text;
                word.start = {frames.first, 25};
                word.end = {frames.second, 25};
                aligned.words.push_back(word);
            }
            // Voice on 10..18 and 23..40 (a hollow of four frames inside
            // "horloge"), then on 50..70 without a break.
            SpeechOnsetReport envelope;
            envelope.media_id = "01K30000000000000000000001";
            envelope.windows_per_second = 50;
            envelope.decode_sample_rate = 16000;
            envelope.speech_level = 1000;
            envelope.noise_floor = 10;
            envelope.levels.assign(160, 10);
            for (int64_t frame = 10; frame < 41; ++frame)
                if (frame < 19 || frame >= 23)
                    envelope.levels[static_cast<size_t>(frame) * 2] =
                        envelope.levels[static_cast<size_t>(frame) * 2 + 1] =
                            1000;
            for (int64_t frame = 50; frame < 70; ++frame)
                envelope.levels[static_cast<size_t>(frame) * 2] =
                    envelope.levels[static_cast<size_t>(frame) * 2 + 1] = 1000;
            // A48 -- voice on 44..47 after "du": the decay of a hold.
            for (int64_t frame = 44; frame < 48; ++frame)
                envelope.levels[static_cast<size_t>(frame) * 2] =
                    envelope.levels[static_cast<size_t>(frame) * 2 + 1] = 1000;
            HesitationReport report;
            report.media_id = "01K30000000000000000000001";
            report.median_stability_milli = 33000;
            HesitationCandidate buried;
            buried.family = "filler";
            buried.start = {232, 100};  // frame 58, inside "souvent"'s tail
            buried.end = {256, 100};    // frame 64, into "dormir"
            buried.stability_milli = 22000;
            buried.previous_word = "souvent";
            buried.next_word = "dormir";
            HesitationCandidate around = buried;
            around.start = {160, 100};  // frame 40: "du" (41..44) inside
            around.end = {180, 100};    // frame 45; the voice runs to 48
            around.previous_word = "horloge";
            around.next_word = "souvent";
            report.candidates = {buried, around};
            InMemoryBackend restartBackend(restartDocument);
            restartBackend.SetSourceTranscript(aligned);
            restartBackend.SetSourceSpeechOnset(envelope);
            restartBackend.SetSourceHesitations(report);
            mcp_json::Value arguments;
            std::string parseFailure;
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000083"})", arguments,
                parseFailure);
            const McpToolCallOutcome seen = cleanRegistry.Call(
                restartBackend, "list_disfluencies", arguments);
            mcp_json::Value view;
            Check(seen.ok && mcp_json::Value::Parse(seen.result_json, view,
                                                    parseFailure),
                  "list_disfluencies with a buried filler and a restart "
                  "parses: " +
                      seen.message);
            const mcp_json::Value* fillers = view.Find("hesitations");
            Check(fillers != nullptr && fillers->AsArray().size() == 2 &&
                      fillers->AsArray()[0].Find("suggested_cut") != nullptr &&
                      fillers->AsArray()[0].Find("suggested_cut")->IsObject() &&
                      fillers->AsArray()[0]
                              .Find("suggested_cut")
                              ->Find("first_frame")
                              ->Dump() == "60" &&
                      fillers->AsArray()[0]
                              .Find("suggested_cut")
                              ->Find("end_frame")
                              ->Dump() == "61",
                  "a filler in the previous word's tail starts at that "
                  "word's aligned end and stops at the next word's attack: " +
                      seen.result_json);
            // A48 -- the hold's decay goes with it, up to the next word;
            // a word inside the hold is named with the cut that takes it.
            if (fillers != nullptr && fillers->AsArray().size() == 2) {
                const mcp_json::Value& inside = fillers->AsArray()[1];
                Check(
                    inside.Find("voice_end_frame") != nullptr &&
                        inside.Find("voice_end_frame")->Dump() == "48" &&
                        inside.Find("words_inside") != nullptr &&
                        inside.Find("words_inside")->Dump() == "[\"du\"]" &&
                        inside.Find("suggested_cut")
                                ->Find("first_frame")
                                ->Dump() == "44" &&
                        inside.Find("suggested_cut")
                                ->Find("end_frame")
                                ->Dump() == "48" &&
                        inside.Find("cut_including_words_inside") != nullptr &&
                        inside.Find("cut_including_words_inside")
                                ->Find("first_frame")
                                ->Dump() == "40" &&
                        inside.Find("cut_including_words_inside")
                                ->Find("end_frame")
                                ->Dump() == "48",
                    "the cut runs to the end of the voice after the hold; "
                    "a word inside it is named, kept whole by the "
                    "cut, taken by the alternative: " +
                        inside.Dump());
            }
            const mcp_json::Value* restarts = view.Find("restarts");
            Check(restarts != nullptr && restarts->AsArray().size() == 1 &&
                      restarts->AsArray()[0].Find("word")->AsString() ==
                          "horloge" &&
                      restarts->AsArray()[0].Find("restart_frame")->Dump() ==
                          "23" &&
                      restarts->AsArray()[0]
                              .Find("suggested_cut")
                              ->Find("first_frame")
                              ->Dump() == "10" &&
                      restarts->AsArray()[0]
                              .Find("suggested_cut")
                              ->Find("end_frame")
                              ->Dump() == "23",
                  "a long aligned word with a hollow inside is a restart, cut "
                  "from its start to the second utterance: " +
                      seen.result_json);
            // A48 -- the clip entered at 46 plays only the decay of the
            // hold at 40..45: the hold is listed for it, its cut clamped
            // to the clip, the word inside the hold left out since the
            // clip does not play it; and the restart, whose first
            // utterance this clip never plays, is not listed.
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000084"})", arguments,
                parseFailure);
            const McpToolCallOutcome later = cleanRegistry.Call(
                restartBackend, "list_disfluencies", arguments);
            mcp_json::Value laterView;
            Check(
                later.ok && mcp_json::Value::Parse(later.result_json, laterView,
                                                   parseFailure),
                "list_disfluencies on the later clip parses: " + later.message);
            const mcp_json::Value* laterFillers = laterView.Find("hesitations");
            const mcp_json::Value* laterRestarts = laterView.Find("restarts");
            Check(
                laterFillers != nullptr &&
                    laterFillers->AsArray().size() == 2 &&
                    laterFillers->AsArray()[1].Find("decay_only") != nullptr &&
                    laterFillers->AsArray()[1].Find("first_frame")->Dump() ==
                        "46" &&
                    laterFillers->AsArray()[1]
                            .Find("voice_end_frame")
                            ->Dump() == "48" &&
                    laterFillers->AsArray()[1]
                            .Find("suggested_cut")
                            ->Find("first_frame")
                            ->Dump() == "46" &&
                    laterFillers->AsArray()[1]
                            .Find("suggested_cut")
                            ->Find("end_frame")
                            ->Dump() == "48" &&
                    laterFillers->AsArray()[1].Find("words_inside") ==
                        nullptr &&
                    laterRestarts != nullptr &&
                    laterRestarts->AsArray().empty(),
                "a clip entered on a hold's decay lists the hold with the "
                "decay to cut, and no restart it does not play: " +
                    later.result_json);
        }

        const std::string beforeCut =
            hesitationBackend.CurrentDocument().SaveToString();
        mcp_json::Value cutArguments;
        mcp_json::Value::Parse(
            R"({"clip_id":"01K30000000000000000000054",)"
            R"("ranges":[{"first_frame":106,"end_frame":108}]})",
            cutArguments, listParseError);
        const McpToolCallOutcome cut = cleanRegistry.Call(
            hesitationBackend, "remove_source_ranges", cutArguments);
        const DocumentTrack& video =
            hesitationBackend.CurrentDocument().sequence.tracks[0];
        const DocumentTrack& audio =
            hesitationBackend.CurrentDocument().sequence.tracks[1];
        Check(cut.ok && video.clips.size() == 2 && audio.clips.size() == 2 &&
                  video.clips[1].source_in == RationalTime{108, 25} &&
                  audio.clips[1].timeline_in == RationalTime{6, 25},
              "the source range is cut from picture and sound alike: " +
                  cut.message);
        std::string undoResult;
        std::string undoName;
        std::string undoMessage;
        Check(
            hesitationBackend.Undo(undoResult, undoName, undoMessage) &&
                hesitationBackend.CurrentDocument().SaveToString() == beforeCut,
            "the source-range cut undoes in one step");
        mcp_json::Value::Parse(
            R"({"clip_id":"01K30000000000000000000054",)"
            R"("ranges":[{"first_frame":108,"end_frame":108}]})",
            cutArguments, listParseError);
        Check(
            !cleanRegistry
                 .Call(hesitationBackend, "remove_source_ranges", cutArguments)
                 .ok,
            "an empty source range is refused");
    }

    // ITW7 -- a sync-locked cutaway track follows the cleanup by default; a
    // track whose sync lock is off stays where it was.
    {
        Document withCutaways = avFixture;
        DocumentTrack cutaways;
        cutaways.id = "01K30000000000000000000055";
        cutaways.kind = "video";
        cutaways.index = 2;
        cutaways.clips = {{"01K30000000000000000000056",
                           "01K30000000000000000000001",
                           {500, 25},
                           {5, 25},
                           {10, 25}}};
        DocumentTrack unsynced = cutaways;
        unsynced.id = "01K30000000000000000000057";
        unsynced.index = 3;
        unsynced.sync_lock = false;
        unsynced.clips[0].id = "01K30000000000000000000058";
        withCutaways.sequence.tracks.push_back(cutaways);
        withCutaways.sequence.tracks.push_back(unsynced);
        InMemoryBackend followBackend(withCutaways);
        followBackend.SetSourceTranscript(transcript);
        const McpToolCallOutcome followed = cleanRegistry.Call(
            followBackend, "clean_disfluencies", avArguments);
        const DocumentClip* moved = followBackend.CurrentDocument().FindClip(
            "01K30000000000000000000056");
        const DocumentClip* stayed = followBackend.CurrentDocument().FindClip(
            "01K30000000000000000000058");
        Check(followed.ok && moved != nullptr && stayed != nullptr &&
                  moved->timeline_in < RationalTime{10, 25} &&
                  stayed->timeline_in == RationalTime{10, 25},
              "a sync-locked cutaway moves back with the cleaned words, an "
              "unsynced one does not: " +
                  followed.message);
    }

    // ---- list_shot_quality: measured, never judged --------------------
    // Forty samples at 4/s. The take is sharp and still except at samples
    // 32 and 33, which is where the timeline's second clip sits.
    ShotQualityReport quality;
    quality.media_id = "01K30000000000000000000001";
    quality.samples_per_second = 4;
    quality.analysis_width = 384;
    quality.analysis_height = 216;
    std::vector<int64_t> allSharpness;
    for (int index = 0; index < 40; ++index) {
        const bool ruined = index == 32 || index == 33;
        ShotQualitySample sample;
        sample.time = RationalTime{index, 4};
        sample.sharpness = ruined ? 900 : 4000;
        sample.motion = ruined ? 130000 : 2000;
        allSharpness.push_back(sample.sharpness);
        quality.samples.push_back(sample);
    }
    quality.median_sharpness = ShotQualityPercentile(allSharpness, 50);

    InMemoryBackend qualityBackend(fixture);
    qualityBackend.SetSourceShotQuality(quality);
    McpToolRegistry qualityRegistry;
    const McpToolCallOutcome qualityOutcome = qualityRegistry.Call(
        qualityBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    Check(qualityOutcome.ok,
          "list_shot_quality reports: " + qualityOutcome.message);
    mcp_json::Value qualityView;
    std::string qualityParseError;
    Check(mcp_json::Value::Parse(qualityOutcome.result_json, qualityView,
                                 qualityParseError),
          "shot quality view parses: " + qualityParseError);
    const mcp_json::Value* qualityClips = qualityView.Find("clips");
    Check(qualityClips && qualityClips->IsArray() &&
              qualityClips->AsArray().size() == 2,
          "both video clips are graded");
    bool sawClean = false;
    bool sawRuined = false;
    if (qualityClips && qualityClips->IsArray()) {
        for (const mcp_json::Value& entry : qualityClips->AsArray()) {
            const mcp_json::Value* sharpness = entry.Find("sharpness");
            const mcp_json::Value* steadiness = entry.Find("steadiness");
            const mcp_json::Value* isClean = entry.Find("clean");
            if (!sharpness || !steadiness || !isClean) continue;
            if (isClean->AsBool() && sharpness->AsString() == "Sharp" &&
                steadiness->AsString() == "Steady")
                sawClean = true;
            if (!isClean->AsBool() && sharpness->AsString() == "Blurry" &&
                steadiness->AsString() == "Shaky")
                sawRuined = true;
        }
    }
    Check(sawClean, "the usable clip grades Sharp and Steady");
    Check(sawRuined,
          "the clip sitting over the ruined samples grades Blurry and Shaky");
    // ITW6 -- a version 3 cache has no camera motion, and says so per clip.
    bool unmeasuredCamera = qualityClips && qualityClips->IsArray();
    if (unmeasuredCamera) {
        for (const mcp_json::Value& entry : qualityClips->AsArray()) {
            const mcp_json::Value* camera = entry.Find("camera");
            unmeasuredCamera = unmeasuredCamera && camera != nullptr &&
                               camera->Find("measured") != nullptr &&
                               !camera->Find("measured")->AsBool();
        }
    }
    Check(unmeasuredCamera,
          "clips of a cache without camera motion report it unmeasured");

    // Occlusion: a clip covered end to end by a higher video track is not a
    // defect to fix, because nobody sees it. The grade must still say it
    // measured badly -- what changes is whether it needs acting on.
    Document coveredFixture = fixture;
    DocumentTrack cover;
    cover.id = "01K30000000000000000000060";
    cover.kind = "video";
    cover.index = 5;  // above track 0, so composited over it
    // Covers the second clip's whole span (timeline 20..30 at 25/s) and only
    // the first half of the first clip's (5..10 of 5..15).
    cover.clips = {{"01K30000000000000000000061",
                    "01K30000000000000000000001",
                    {0, 25},
                    {10, 25},
                    {20, 25}},
                   {"01K30000000000000000000062",
                    "01K30000000000000000000001",
                    {0, 25},
                    {5, 25},
                    {5, 25}}};
    coveredFixture.sequence.tracks.push_back(cover);

    InMemoryBackend coveredBackend(coveredFixture);
    // Soft only where the two clips actually read -- source seconds 4.0 and
    // 8.0, i.e. samples 16-17 and 32-33 on the 4/s grid. Making the whole
    // source soft instead would move its own median down onto the soft value
    // and grade everything Sharp, which is the documented cost of a
    // source-relative grade, not the thing under test here.
    ShotQualityReport poor;
    poor.media_id = "01K30000000000000000000001";
    poor.samples_per_second = 4;
    poor.analysis_width = 384;
    poor.analysis_height = 216;
    std::vector<int64_t> poorSharpness;
    for (int index = 0; index < 40; ++index) {
        ShotQualitySample sample;
        sample.time = RationalTime{index, 4};
        const bool insideAClip =
            index == 16 || index == 17 || index == 32 || index == 33;
        sample.sharpness = insideAClip ? 800 : 4000;
        sample.motion = 2000;
        poorSharpness.push_back(sample.sharpness);
        poor.samples.push_back(sample);
    }
    poor.median_sharpness = ShotQualityPercentile(poorSharpness, 50);
    poor.median_motion = 2000;
    coveredBackend.SetSourceShotQuality(poor);

    const McpToolCallOutcome coveredOutcome = qualityRegistry.Call(
        coveredBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    mcp_json::Value coveredView;
    std::string coveredParseError;
    Check(coveredOutcome.ok &&
              mcp_json::Value::Parse(coveredOutcome.result_json, coveredView,
                                     coveredParseError),
          "the covered timeline reports: " + coveredParseError);
    int fullyCovered = 0;
    int needsAttention = 0;
    int gradedBad = 0;
    const mcp_json::Value* coveredClips = coveredView.Find("clips");
    if (coveredClips && coveredClips->IsArray()) {
        for (const mcp_json::Value& entry : coveredClips->AsArray()) {
            const mcp_json::Value* isClean = entry.Find("clean");
            const mcp_json::Value* covered = entry.Find("fully_covered");
            const mcp_json::Value* attention = entry.Find("needs_attention");
            if (!isClean || !covered || !attention) continue;
            if (!isClean->AsBool()) ++gradedBad;
            if (covered->AsBool()) ++fullyCovered;
            if (attention->AsBool()) ++needsAttention;
        }
    }
    Check(gradedBad >= 2,
          "both clips of the covered fixture still measure badly");
    Check(fullyCovered == 1,
          "exactly the clip covered end to end reports fully_covered");
    Check(needsAttention == gradedBad - 1,
          "the covered one drops out of needs_attention while the partly "
          "visible one stays: occlusion changes what to act on, never the "
          "grade");

    // A source that was never analysed must read as unknown, never as a
    // pass: a caller filtering on `clean` would otherwise cut with it.
    InMemoryBackend blindBackend(fixture);
    const McpToolCallOutcome blindOutcome = qualityRegistry.Call(
        blindBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    mcp_json::Value blindView;
    std::string blindParseError;
    Check(blindOutcome.ok && mcp_json::Value::Parse(blindOutcome.result_json,
                                                    blindView, blindParseError),
          "an unanalysed timeline still reports: " + blindParseError);
    const mcp_json::Value* blindClips = blindView.Find("clips");
    const mcp_json::Value* blindUnknown = blindView.Find("unanalyzed");
    Check(blindClips && blindClips->IsArray() && blindClips->AsArray().empty(),
          "nothing is graded without an analysis");
    Check(blindUnknown && blindUnknown->IsArray() &&
              blindUnknown->AsArray().size() == 2,
          "every unmeasured clip is named as unmeasured rather than omitted");

    // AGENT-2026-09 A1 -- ROADMAP.md. The stdio transport shares the
    // envelope: the same requests through a pipe get the same answers, one
    // line each, in order; a notification and a blank line get none, a
    // line ending in CR LF is read like one ending in LF, and an unknown
    // method is answered with a JSON-RPC error rather than a closed stream.
    // AX6 -- docs/roadmap/AX-2026-09.md. The map of the catalog the server
    // gives at connection names tools a client loads by name: every name in
    // it must be one, or the map sends an agent after a tool that is gone.
    {
        McpServer mapServer(backend);
        std::string answer;
        mapServer.HandleJsonRpc(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
            R"({"protocolVersion":"2025-06-18"}})",
            answer);
        mcp_json::Value parsed;
        std::string parseError;
        mcp_json::Value::Parse(answer, parsed, parseError);
        const mcp_json::Value* result = parsed.Find("result");
        const std::string instructions =
            result != nullptr && result->Find("instructions") != nullptr
                ? result->Find("instructions")->AsString()
                : std::string();
        const size_t map = instructions.find("Tools by task:");
        Check(map != std::string::npos, "the instructions carry the map");
        // PRD18 -- ROADMAP.md. Claude Code keeps the first 2,048 characters
        // of a server's instructions, counted as JavaScript counts a string
        // (UTF-16 code units), and drops the rest: what lies beyond never
        // reaches the model. Everything the instructions are for must fit.
        size_t units = 0;
        for (const char byte : instructions) {
            const auto value = static_cast<unsigned char>(byte);
            if ((value & 0xC0) != 0x80) units += value >= 0xF0 ? 2 : 1;
        }
        Check(units <= 2048,
              "the instructions fit in the 2,048 characters a "
              "client keeps: " +
                  std::to_string(units));
        // What an injected argument says (async, expected_revision,
        // idempotency_key) is said on the argument, not here; what holds
        // for every tool is said here, not on each.
        for (const char* kept :
             {"`describe`", "`delta`", "`shifted`", "`get_procedure`",
              "one undo step", "unique prefix or an alias",
              "measure, never judge", "Tools by task:", "get_editor_view"}) {
            Check(instructions.find(kept) != std::string::npos,
                  std::string("the instructions still say ") + kept);
        }
        const std::string section =
            map == std::string::npos ? std::string() : instructions.substr(map);
        const McpToolRegistry catalog;
        std::set<std::string> named;
        for (size_t at = 0; at < section.size();) {
            const size_t begin =
                section.find_first_of("abcdefghijklmnopqrstuvwxyz", at);
            if (begin == std::string::npos) break;
            size_t end = begin;
            while (end < section.size() &&
                   (std::islower(static_cast<unsigned char>(section[end])) ||
                    section[end] == '_'))
                ++end;
            const std::string word = section.substr(begin, end - begin);
            if (word.find('_') != std::string::npos) named.insert(word);
            at = end + 1;
        }
        std::string unknown;
        for (const std::string& name : named) {
            const bool known = std::any_of(
                catalog.Tools().begin(), catalog.Tools().end(),
                [&](const McpTool& tool) { return tool.name == name; });
            if (!known) unknown += " " + name;
        }
        Check(named.size() > 30 && unknown.empty(),
              "every tool the map names is in the catalog (" +
                  std::to_string(named.size()) + " named):" + unknown);
        // PRD18 -- the procedures are a capability the server declares.
        const mcp_json::Value* capabilities =
            result != nullptr ? result->Find("capabilities") : nullptr;
        Check(capabilities != nullptr &&
                  capabilities->Find("tools") != nullptr &&
                  capabilities->Find("prompts") != nullptr,
              "initialize declares tools and prompts: " + answer);
    }
    // PRD13b -- ROADMAP.md. A relay in front of the server waits for an
    // answer exactly when the server writes one (McpEndpoint.h): the two
    // read every shape of message alike, batches and client responses
    // included.
    {
        McpServer replyServer(backend);
        for (const std::string& message : std::vector<std::string>{
                 R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                 R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                 R"({"jsonrpc":"2.0","id":2,"result":{}})",
                 R"({"jsonrpc":"2.0","id":3,"error":{"code":1,"message":"x"}})",
                 R"({"jsonrpc":"2.0","result":{}})",
                 R"({"id":4})",
                 R"({"jsonrpc":"1.0","id":5,"method":"ping"})",
                 R"({"jsonrpc":"2.0","method":7})",
                 "42",
                 R"("text")",
                 "not json",
                 "[]",
                 "[1]",
                 R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])",
                 R"([{"jsonrpc":"2.0","id":6,"result":{}}])",
                 R"([{"jsonrpc":"2.0","id":7,"result":{}},)"
                 R"({"jsonrpc":"2.0","id":8,"method":"ping"}])",
                 R"([{"jsonrpc":"2.0","id":9,"method":"initialize"}])",
             }) {
            std::string answer;
            Check(replyServer.HandleJsonRpc(message, answer) ==
                      McpJsonRpcGetsAnswer(message),
                  "the server and a relay agree on whether this is "
                  "answered: " +
                      message);
        }
        std::string answer;
        Check(replyServer.HandleJsonRpcMessage(
                  R"({"jsonrpc":"2.0","id":2,"result":{}})", answer) ==
                      McpReply::kClientResponse &&
                  answer.find("\"id\":null") != std::string::npos &&
                  answer.find("-32600") != std::string::npos,
              "a client's response is told apart, with an error to send "
              "over HTTP: " +
                  answer);
        Check(replyServer.HandleJsonRpcMessage(
                  R"([{"jsonrpc":"2.0","method":"a"},{"jsonrpc":"2.0",)"
                  R"("method":"b"}])",
                  answer) == McpReply::kNone &&
                  answer.empty(),
              "a batch of notifications calls for nothing");
        Check(replyServer.HandleJsonRpcMessage(
                  R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
                  R"({"jsonrpc":"2.0","id":"two","method":"nope"}])",
                  answer) == McpReply::kAnswer &&
                  answer.rfind(R"([{"jsonrpc":"2.0","id":1,"result":{}},)",
                               0) == 0 &&
                  answer.find(R"("id":"two","error":{"code":-32601)") !=
                      std::string::npos,
              "a batch is answered in order, each answer as alone: " + answer);
        int port = -1;
        std::string error;
        Check(ParseMcpPort("8765", port, error) && port == 8765 &&
                  ParseMcpPort("1", port, error) && port == 1 &&
                  ParseMcpPort("65535", port, error) && port == 65535 &&
                  ParseMcpPort("08080", port, error) && port == 8080 &&
                  ParseMcpPort("0", port, error) && port == 0,
              "a port is a number from 1 to 65535, or 0 for a free one");
        for (const std::string& text :
             {"", "abc", "65536", "99999", "123456", "-1", "+80", " 80", "80 ",
              "8.0", "80x"}) {
            port = 7;
            error.clear();
            Check(!ParseMcpPort(text, port, error) && port == 7 &&
                      error.find("1 to 65535") != std::string::npos &&
                      error.find("not '" + text + "'") != std::string::npos,
                  "'" + text + "' is refused by name: " + error);
        }
    }
    // PRD18 -- ROADMAP.md (ACP4). One prompt per procedure, for a person who
    // starts one by hand: its SKILL.md as the user's message, after a
    // sentence that says where its files are read.
    {
        McpServer promptServer(backend);
        const auto ask = [&](const std::string& request) {
            std::string answer;
            promptServer.HandleJsonRpc(request, answer);
            mcp_json::Value parsed;
            std::string parseError;
            mcp_json::Value::Parse(answer, parsed, parseError);
            return parsed;
        };
        const mcp_json::Value listed =
            ask(R"({"jsonrpc":"2.0","id":1,"method":"prompts/list"})");
        const mcp_json::Value* prompts =
            listed.Find("result") != nullptr
                ? listed.Find("result")->Find("prompts")
                : nullptr;
        const std::vector<Procedure> procedures = ListProcedures();
        Check(prompts != nullptr && prompts->IsArray() &&
                  prompts->AsArray().size() == procedures.size(),
              "prompts/list gives one prompt per procedure: " + listed.Dump());
        for (size_t index = 0;
             prompts != nullptr && prompts->IsArray() &&
             index < prompts->AsArray().size() && index < procedures.size();
             ++index) {
            const mcp_json::Value& prompt = prompts->AsArray()[index];
            Check(
                prompt.Find("name") != nullptr &&
                    prompt.Find("name")->AsString() == procedures[index].name &&
                    prompt.Find("description") != nullptr &&
                    prompt.Find("description")->AsString() ==
                        procedures[index].description &&
                    prompt.Find("arguments") == nullptr,
                "a prompt is a procedure's name and description, without "
                "arguments: " +
                    prompt.Dump().substr(0, 160));
        }
        const mcp_json::Value got =
            ask(R"({"jsonrpc":"2.0","id":2,"method":"prompts/get","params":)"
                R"({"name":"motion-design"}})");
        const mcp_json::Value* result = got.Find("result");
        const mcp_json::Value* messages =
            result != nullptr ? result->Find("messages") : nullptr;
        ProcedureText skill;
        std::string errorName;
        std::string message;
        ReadProcedureFile("motion-design", "SKILL.md", skill, errorName,
                          message);
        const mcp_json::Value* first = messages != nullptr &&
                                               messages->IsArray() &&
                                               messages->AsArray().size() == 1
                                           ? &messages->AsArray()[0]
                                           : nullptr;
        const mcp_json::Value* content =
            first != nullptr ? first->Find("content") : nullptr;
        const std::string text =
            content != nullptr && content->Find("text") != nullptr
                ? content->Find("text")->AsString()
                : std::string();
        Check(first != nullptr && first->Find("role") != nullptr &&
                  first->Find("role")->AsString() == "user" &&
                  content->Find("type") != nullptr &&
                  content->Find("type")->AsString() == "text" &&
                  !skill.text.empty() && text.size() > skill.text.size() &&
                  text.compare(text.size() - skill.text.size(),
                               skill.text.size(), skill.text) == 0 &&
                  text.find("get_procedure") < text.find("---") &&
                  result->Find("description") != nullptr &&
                  !result->Find("description")->AsString().empty(),
              "prompts/get gives the SKILL.md as the user's message, after "
              "the sentence that names get_procedure: " +
                  text.substr(0, 300));
        const mcp_json::Value unknown =
            ask(R"({"jsonrpc":"2.0","id":3,"method":"prompts/get","params":)"
                R"({"name":"monter-un-film"}})");
        const mcp_json::Value* unknownError = unknown.Find("error");
        Check(unknownError != nullptr &&
                  unknownError->Find("code")->Dump() == "-32602" &&
                  unknownError->Find("message")->AsString().find(
                      "monter-une-itw") != std::string::npos &&
                  unknown.Find("id")->Dump() == "3",
              "an unknown prompt is invalid params, naming the known ones: " +
                  unknown.Dump());
        const mcp_json::Value nameless = ask(
            R"({"jsonrpc":"2.0","id":4,"method":"prompts/get","params":{}})");
        Check(
            nameless.Find("error") != nullptr &&
                nameless.Find("error")->Find("code")->Dump() == "-32602",
            "prompts/get without a name is invalid params: " + nameless.Dump());
        const mcp_json::Value nope =
            ask(R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})");
        Check(nope.Find("error") != nullptr &&
                  nope.Find("error")->Find("code")->Dump() == "-32601",
              "no resources are served: an unknown method stays -32601");
    }
    {
        McpServer stdioServer(backend);
        const std::string requests =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
            R"({"protocolVersion":"2024-11-05"}})"
            "\n"
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
            "\n\n"
            R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
            "\r\n"
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":)"
            R"({"name":"describe","arguments":{}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":4,"method":"nope"})"
            "\n";
        std::FILE* input =
            fmemopen(const_cast<char*>(requests.data()), requests.size(), "r");
        char* written = nullptr;
        size_t writtenSize = 0;
        std::FILE* output = open_memstream(&written, &writtenSize);
        Check(input != nullptr && output != nullptr,
              "memory streams open for the stdio transport");
        const int served = stdioServer.ServeStdio(input, output);
        if (input) std::fclose(input);
        if (output) std::fclose(output);
        const std::string answers(written ? written : "", writtenSize);
        std::free(written);
        Check(served == 0, "the stdio loop ends cleanly at end of input");
        std::vector<mcp_json::Value> lines;
        std::istringstream stream(answers);
        std::string line;
        while (std::getline(stream, line)) {
            mcp_json::Value parsed;
            std::string parseError;
            Check(mcp_json::Value::Parse(line, parsed, parseError),
                  "each stdio answer is one JSON line: " + parseError);
            lines.push_back(parsed);
        }
        Check(lines.size() == 4,
              "four answers for four requests over stdio, none for the "
              "notification or the blank line: " +
                  std::to_string(lines.size()));
        if (lines.size() == 4) {
            const auto id = [&](size_t index) {
                const mcp_json::Value* found = lines[index].Find("id");
                return found ? found->Dump() : std::string("(absent)");
            };
            Check(id(0) == "1" && id(1) == "2" && id(2) == "3" && id(3) == "4",
                  "stdio answers keep their request ids, in order");
            Check(lines[0].Find("result") &&
                      lines[0].Find("result")->Find("protocolVersion") &&
                      lines[0].Find("result")
                              ->Find("protocolVersion")
                              ->AsString() == "2024-11-05",
                  "initialize over stdio answers the requested version");
            Check(
                lines[1].Find("result") && lines[1].Find("result")->IsObject(),
                "ping answers an empty object");
            const mcp_json::Value* content =
                lines[2].Find("result")
                    ? lines[2].Find("result")->Find("content")
                    : nullptr;
            mcp_json::Value payload;
            std::string parseError;
            Check(content && content->IsArray() &&
                      !content->AsArray().empty() &&
                      content->AsArray()[0].Find("text") &&
                      mcp_json::Value::Parse(
                          content->AsArray()[0].Find("text")->AsString(),
                          payload, parseError) &&
                      payload.Find("sequence") != nullptr,
                  "a tool call over stdio answers like one over HTTP: " +
                      parseError);
            const mcp_json::Value* unknown = lines[3].Find("error");
            Check(unknown && unknown->Find("code") &&
                      unknown->Find("code")->Dump() == "-32601",
                  "an unknown method over stdio is a JSON-RPC error");
        }
    }

    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "MCP server starts on an ephemeral port: " + startError);
    Check(server.Port() != 0, "server reports its bound port");

    // ---- tools/list ----
    const std::string listRequest =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    const std::string listResponse =
        HttpPostJson(server.Port(), "/mcp", listRequest);
    Check(StatusLine(listResponse) == "HTTP/1.1 200 OK",
          "tools/list returns HTTP 200");
    mcp_json::Value listBody;
    std::string parseError;
    Check(mcp_json::Value::Parse(HttpBody(listResponse), listBody, parseError),
          "tools/list body is valid JSON: " + parseError);
    const mcp_json::Value* toolsField =
        listBody.Find("result") ? listBody.Find("result")->Find("tools")
                                : nullptr;
    Check(toolsField && toolsField->IsArray() &&
              toolsField->AsArray().size() > 30,
          "tools/list exposes the full catalog (>30 tools)");
    bool sawInsertClip = false;
    bool sawClearClips = false;
    bool sawTrimClip = false;
    bool sawSetColorManagement = false;
    bool sawSetClipOpacity = false;
    bool sawSetClipAudio = false;
    bool sawContactSheet = false;
    bool sawCutSheet = false;
    bool sawUndo = false;
    int multicamTools = 0;
    if (toolsField) {
        for (const mcp_json::Value& tool : toolsField->AsArray()) {
            const mcp_json::Value* name = tool.Find("name");
            if (!name) continue;
            if (name->AsString() == "insert_clip") sawInsertClip = true;
            if (name->AsString() == "clear_clips") sawClearClips = true;
            if (name->AsString() == "trim_clip") sawTrimClip = true;
            if (name->AsString() == "set_color_management")
                sawSetColorManagement = true;
            if (name->AsString() == "set_clip_opacity")
                sawSetClipOpacity = true;
            if (name->AsString() == "set_clip_audio") sawSetClipAudio = true;
            if (name->AsString() == "contact_sheet") sawContactSheet = true;
            if (name->AsString() == "cut_sheet") sawCutSheet = true;
            if (name->AsString() == "undo") sawUndo = true;
            // F1.5 -- the multicam tools are offered now, and the three that
            // edit take timeline_id like every other editing tool (ITW7);
            // PL3 -- the one that reads takes it too, to read a timeline
            // without activating it.
            if (name->AsString() == "add_multicam_group" ||
                name->AsString() == "set_multicam_active_angle" ||
                name->AsString() == "remove_multicam_group" ||
                name->AsString() == "list_multicam_groups") {
                ++multicamTools;
                const mcp_json::Value* schema = tool.Find("inputSchema");
                const mcp_json::Value* properties =
                    (schema != nullptr) ? schema->Find("properties") : nullptr;
                const bool scoped = (properties != nullptr) &&
                                    properties->Find("timeline_id") != nullptr;
                Check(scoped, name->AsString() + " accepts timeline_id");
            }
        }
    }
    Check(sawInsertClip, "tools/list includes insert_clip");
    Check(sawClearClips, "tools/list includes clear_clips");
    Check(sawTrimClip, "tools/list includes trim_clip");
    Check(sawSetColorManagement, "tools/list includes set_color_management");
    Check(sawSetClipOpacity, "tools/list includes set_clip_opacity");
    Check(sawSetClipAudio, "tools/list includes set_clip_audio");
    Check(sawContactSheet, "tools/list includes contact_sheet");
    Check(sawCutSheet, "tools/list includes cut_sheet");
    Check(sawUndo, "tools/list includes undo");
    Check(multicamTools == 4, "tools/list offers the four multicam tools");

    // ---- tools/call: trim_clip, compared against a direct EditLog::Apply ----
    // Exercise the ID resolver: "01K3000000000000000000000" + "3" is a full
    // ID; a short unambiguous prefix should resolve identically.
    const std::string trimRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K3000000000000000000000)"
        R"(3","edge":"Tail","delta":{"value":-1,"rate":25}}}})";
    const std::string trimResponse =
        HttpPostJson(server.Port(), "/mcp", trimRequest);
    mcp_json::Value trimBody;
    Check(mcp_json::Value::Parse(HttpBody(trimResponse), trimBody, parseError),
          "tools/call trim_clip body is valid JSON: " + parseError);
    const mcp_json::Value* trimResult = trimBody.Find("result");
    Check(trimResult != nullptr, "tools/call trim_clip returns a result");
    if (trimResult) {
        const mcp_json::Value* isError = trimResult->Find("isError");
        Check(isError && isError->IsBool() && isError->AsBool() == false,
              "tools/call trim_clip is not an error");
    }

    Document expected = fixture;
    EditLog expectedLog;
    EditError expectedError = EditError::None;
    std::string expectedMessage;
    const Operation expectedTrim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    Check(expectedLog.Apply(expected, expectedTrim, expectedError,
                            expectedMessage),
          "reference direct EditLog::Apply(trim) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call trim_clip changes the document exactly as a direct "
          "EditLog::Apply/--apply-op call would");

    // ---- tools/call: arbitrary atomic multi-clear ----
    const std::string clearRequest =
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call",)"
        R"("params":{"name":"clear_clips","arguments":{"clip_ids":[)"
        R"("01K30000000000000000000003",)"
        R"("01K30000000000000000000004"]}}})";
    const std::string clearResponse =
        HttpPostJson(server.Port(), "/mcp", clearRequest);
    Check(clearResponse.find("\"isError\":false") != std::string::npos,
          "tools/call clear_clips is not an error");
    Check(expectedLog.Apply(expected,
                            ClearClipsOperation{{"01K30000000000000000000003",
                                                 "01K30000000000000000000004"},
                                                {}},
                            expectedError, expectedMessage),
          "reference direct EditLog::Apply(clear_clips) succeeds: " +
              expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call clear_clips matches a direct atomic operation");

    // ---- tools/call: insert_clip ----
    // insert_clip's clip_id is engine-generated (ApplyInsert's own
    // GenerateUlid(), same as a human/CLI insert never supplying one), so
    // the MCP call and an independently-constructed reference operation
    // cannot agree on it by chance. Discover the ID the server actually
    // assigned, then replay a reference InsertClipOperation with that exact
    // ID for a true byte-for-byte comparison -- this is the same reasoning
    // ApplyInsert itself documents: the ID is retained "for redo/inverse
    // identity", not invented independently on each replay.
    std::vector<Ulid> clipIdsBeforeInsert;
    for (const DocumentTrack& track : backend.CurrentDocument().sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            clipIdsBeforeInsert.push_back(clip.id);
    const std::string insertRequest =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
        R"("params":{"name":"insert_clip","arguments":{)"
        R"("track_id":"01K30000000000000000000002",)"
        R"("source_id":"01K30000000000000000000001",)"
        R"("source_in":{"value":0,"rate":25},)"
        R"("duration":{"value":5,"rate":25},)"
        R"("timeline_in":{"value":0,"rate":25}}}})";
    const std::string insertResponse =
        HttpPostJson(server.Port(), "/mcp", insertRequest);
    Check(insertResponse.find("\"isError\":false") != std::string::npos,
          "tools/call insert_clip is not an error");

    Ulid insertedClipId;
    for (const DocumentTrack& track :
         backend.CurrentDocument().sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (std::find(clipIdsBeforeInsert.begin(),
                          clipIdsBeforeInsert.end(),
                          clip.id) == clipIdsBeforeInsert.end())
                insertedClipId = clip.id;
        }
    }
    Check(!insertedClipId.empty(),
          "insert_clip created exactly one new, previously-unseen clip_id");

    Operation expectedInsert = InsertClipOperation{"01K30000000000000000000002",
                                                   "01K30000000000000000000001",
                                                   {0, 25},
                                                   {5, 25},
                                                   {0, 25},
                                                   insertedClipId,
                                                   {}};
    Check(
        expectedLog.Apply(expected, expectedInsert, expectedError,
                          expectedMessage),
        "reference direct EditLog::Apply(insert) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call insert_clip changes the document exactly as a direct "
          "EditLog::Apply/--apply-op call would");

    const std::string opacityRequest =
        std::string(R"({"jsonrpc":"2.0","id":11,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_opacity","arguments":{"clip_id":")" +
        insertedClipId + R"(","opacity":{"num":3,"den":5}}}})";
    const std::string opacityResponse =
        HttpPostJson(server.Port(), "/mcp", opacityRequest);
    Check(opacityResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_opacity is not an error");
    Check(expectedLog.Apply(expected,
                            SetClipOpacityOperation{insertedClipId, {3, 5}},
                            expectedError, expectedMessage),
          "reference direct EditLog::Apply(opacity) succeeds: " +
              expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_opacity matches a direct operation");

    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The blur the Effets tab drops
    // and the Inspector sets, through the tool: the same operation, the
    // fraction reduced alike, and a blur past the bound refused by name.
    const std::string blurRequest =
        std::string(R"({"jsonrpc":"2.0","id":12,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_blur","arguments":{"clip_id":")" +
        insertedClipId + R"(","blur":{"num":20,"den":2}}}})";
    const std::string blurResponse =
        HttpPostJson(server.Port(), "/mcp", blurRequest);
    Check(blurResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_blur is not an error: " + blurResponse);
    Check(expectedLog.Apply(expected,
                            SetClipBlurOperation{insertedClipId, {10, 1}},
                            expectedError, expectedMessage),
          "reference direct EditLog::Apply(blur) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_blur matches a direct operation, reduced to 10/1");
    const std::string tooBlurred =
        std::string(R"({"jsonrpc":"2.0","id":13,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_blur","arguments":{"clip_id":")" +
        insertedClipId + R"(","blur":{"num":201,"den":1}}}})";
    const std::string tooBlurredResponse =
        HttpPostJson(server.Port(), "/mcp", tooBlurred);
    Check(tooBlurredResponse.find("\"isError\":true") != std::string::npos &&
              tooBlurredResponse.find("blur outside") != std::string::npos,
          "set_clip_blur refuses a blur past the bound: " + tooBlurredResponse);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "a refused blur leaves the document as it was");

    // R3 -- docs/roadmap/REEL-2026-09.md. The MCP tool and --apply-op reach
    // the same operation: what the mouse would do, a script does.
    const std::string speedRequest =
        std::string(R"({"jsonrpc":"2.0","id":31,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_speed","arguments":{"clip_id":")" +
        insertedClipId + R"(","speed":{"num":1,"den":2}}}})";
    const std::string speedResponse =
        HttpPostJson(server.Port(), "/mcp", speedRequest);
    Check(speedResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_speed is not an error: " + speedResponse);
    Check(
        expectedLog.Apply(expected,
                          SetClipSpeedOperation{insertedClipId, {1, 2}},
                          expectedError, expectedMessage),
        "reference direct EditLog::Apply(speed) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_speed matches a direct operation");
    Check(expectedLog.Undo(expected, expectedError, expectedMessage),
          "the reference log steps back off the speed");
    const std::string unspeedRequest =
        std::string(R"({"jsonrpc":"2.0","id":32,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_speed","arguments":{"clip_id":")" +
        insertedClipId + R"(","speed":{"num":1,"den":1}}}})";
    Check(HttpPostJson(server.Port(), "/mcp", unspeedRequest)
                  .find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_speed back to 1/1 is not an error");
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "and the document is byte-identical to the one before the speed");

    // R3, criterion 4 -- a ramp through the same tool, its key ids
    // generated when left out; a speed and a ramp at once, and a curve the
    // integral cannot follow, are refused before anything is applied.
    const std::string rampRequest =
        std::string(R"({"jsonrpc":"2.0","id":33,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_speed","arguments":{"clip_id":")" +
        insertedClipId +
        R"(","speed_ramp":[{"time":{"value":0,"rate":25},"value":{"num":1,"den":1}},)"
        R"({"time":{"value":4,"rate":25},"value":{"num":1,"den":2},)"
        R"("interpolation":"hold"}]}}})";
    const std::string rampResponse =
        HttpPostJson(server.Port(), "/mcp", rampRequest);
    Check(rampResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_speed with a ramp is not an error: " +
              rampResponse);
    const DocumentClip* ramped =
        backend.CurrentDocument().FindClip(insertedClipId);
    Check(
        ramped != nullptr && ramped->speed_ramp.size() == 2 &&
            !ramped->speed_ramp[0].id.empty() &&
            ramped->speed_ramp[0].id != ramped->speed_ramp[1].id &&
            ramped->speed_ramp[1].interpolation == KeyframeInterpolation::Hold,
        "the clip carries two keys with ids of their own");
    const std::string bothRequest =
        std::string(R"({"jsonrpc":"2.0","id":34,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_speed","arguments":{"clip_id":")" +
        insertedClipId + R"(","speed":{"num":1,"den":2},"speed_ramp":[]}}})";
    Check(
        HttpPostJson(server.Port(), "/mcp", bothRequest).find("exactly one") !=
            std::string::npos,
        "a speed and a ramp at once are refused");
    const std::string easedRequest =
        std::string(R"({"jsonrpc":"2.0","id":35,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_speed","arguments":{"clip_id":")" +
        insertedClipId +
        R"(","speed_ramp":[{"time":{"value":0,"rate":25},"value":{"num":1,"den":1},)"
        R"("interpolation":"cubic-bezier"},{"time":{"value":4,"rate":25},)"
        R"("value":{"num":1,"den":2}}]}}})";
    Check(HttpPostJson(server.Port(), "/mcp", easedRequest)
                  .find("'linear' or 'hold'") != std::string::npos,
          "a cubic-bezier ramp is refused by name");
    Check(
        HttpPostJson(server.Port(), "/mcp", unspeedRequest)
                    .find("\"isError\":false") != std::string::npos &&
            backend.CurrentDocument().SaveToString() == expected.SaveToString(),
        "back to 1/1, the ramp is gone and every byte is back");

    const std::string audioRequest =
        std::string(R"({"jsonrpc":"2.0","id":12,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_audio","arguments":{"clip_id":")" +
        R"(01K30000000000000000000006","gain_db":{"num":10,"den":1},)"
        R"("fade_in":{"value":2,"rate":25},"fade_out":{"value":3,"rate":25}}}})";
    const std::string audioResponse =
        HttpPostJson(server.Port(), "/mcp", audioRequest);
    Check(audioResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_audio is not an error");
    Check(
        expectedLog.Apply(
            expected,
            SetClipAudioOperation{
                "01K30000000000000000000006", {10, 1}, {2, 25}, {3, 25}},
            expectedError, expectedMessage),
        "reference direct EditLog::Apply(audio) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_audio matches a direct operation");

    // ---- tools/call: project color state through the shared operation ----
    const std::string colorRequest =
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call",)"
        R"("params":{"name":"set_color_management","arguments":{)"
        R"("enabled":true,"input_gamut":"sony_sgamut3_cine",)"
        R"("input_transfer":"sony_slog3","input_ycbcr_matrix":"bt709",)"
        R"("input_range":"full","working_gamut":"acescct",)"
        R"("output_gamut":"rec2020","output_transfer":"hlg"}}})";
    const std::string colorResponse =
        HttpPostJson(server.Port(), "/mcp", colorRequest);
    Check(colorResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_color_management is not an error");
    ColorManagementSettings colorSettings;
    colorSettings.enabled = true;
    colorSettings.input_gamut = "sony_sgamut3_cine";
    colorSettings.input_transfer = "sony_slog3";
    colorSettings.input_ycbcr_matrix = "bt709";
    colorSettings.input_range = "full";
    colorSettings.working_gamut = "acescct";
    colorSettings.output_gamut = "rec2020";
    colorSettings.output_transfer = "hlg";
    Check(
        expectedLog.Apply(expected, SetColorManagementOperation{colorSettings},
                          expectedError, expectedMessage),
        "reference direct EditLog::Apply(color) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_color_management matches a direct operation");

    // set_color_management changes the fields it is given and keeps the
    // others: switching management on no longer means restating the whole
    // pipeline, and restating part of it no longer switches it off.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& json) {
            mcp_json::Value args;
            std::string parseError;
            mcp_json::Value::Parse(json, args, parseError);
            return registry.Call(backend, "set_color_management", args);
        };
        const auto& settings = [&]() -> const ColorManagementSettings& {
            return backend.CurrentDocument().color_management;
        };
        const auto on = call(R"({"enabled":true})");
        Check(on.ok && settings().enabled &&
                  settings().input_transfer == "rec709" &&
                  settings().working_gamut == "acescct",
              "enabled alone switches management on: " + on.message);
        const auto log = call(
            R"({"input_gamut":"sony_sgamut3_cine","input_transfer":"sony_slog3"})");
        Check(log.ok && settings().enabled &&
                  settings().input_transfer == "sony_slog3" &&
                  settings().input_gamut == "sony_sgamut3_cine" &&
                  settings().output_transfer == "rec709",
              "a partial setting keeps management on and the other fields: " +
                  log.message);
        const auto off = call(R"({"enabled":false})");
        Check(off.ok && !settings().enabled &&
                  settings().input_transfer == "sony_slog3",
              "switching off keeps the pipeline for next time: " + off.message);
        const auto wrong = call(R"({"input_transfer":"slog9"})");
        Check(!wrong.ok && settings().input_transfer == "sony_slog3",
              "an unknown value is still refused: " + wrong.message);
    }

    // ---- undo/redo tools ----
    const std::string undoRequest =
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"undo","arguments":{}}})";
    HttpPostJson(server.Port(), "/mcp", undoRequest);
    EditError undoError = EditError::None;
    std::string undoMessage;
    Check(expectedLog.Undo(expected, undoError, undoMessage),
          "reference direct EditLog::Undo succeeds: " + undoMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "the undo tool matches a direct EditLog::Undo call");

    // ---- error path: unknown clip leaves the document untouched ----
    const std::string beforeError = backend.CurrentDocument().SaveToString();
    const std::string badRequest =
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K39999999999999999999999","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string badResponse =
        HttpPostJson(server.Port(), "/mcp", badRequest);
    mcp_json::Value badBody;
    Check(mcp_json::Value::Parse(HttpBody(badResponse), badBody, parseError),
          "error response body is valid JSON: " + parseError);
    const mcp_json::Value* badResult = badBody.Find("result");
    bool badIsError = false;
    bool badTextIsEnvelope = false;
    if (badResult) {
        const mcp_json::Value* isError = badResult->Find("isError");
        badIsError = isError && isError->IsBool() && isError->AsBool();
        const mcp_json::Value* content = badResult->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty()) {
            const mcp_json::Value* text = content->AsArray()[0].Find("text");
            mcp_json::Value envelope;
            std::string envelopeError;
            if (text != nullptr && text->IsString() &&
                mcp_json::Value::Parse(text->AsString(), envelope,
                                       envelopeError) &&
                envelope.IsObject()) {
                const mcp_json::Value* ok = envelope.Find("ok");
                const mcp_json::Value* error = envelope.Find("error");
                const mcp_json::Value* detail = envelope.Find("detail");
                badTextIsEnvelope = ok != nullptr && ok->IsBool() &&
                                    !ok->AsBool() && error != nullptr &&
                                    error->IsString() &&
                                    error->AsString() == "ValidationFailed" &&
                                    detail != nullptr && detail->IsString() &&
                                    !detail->AsString().empty();
            }
        }
    }
    Check(badIsError,
          "an unknown clip_id is reported as isError, not a "
          "JSON-RPC protocol error");
    Check(badTextIsEnvelope,
          "a refused tools/call exposes an ok/error/detail JSON object");
    Check(backend.CurrentDocument().SaveToString() == beforeError,
          "a refused tool call leaves the document byte-identical");

    // ---- argument validation: unknown key is rejected by name ----
    const std::string unknownKeyRequest =
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25},"bogus":true}}})";
    const std::string unknownKeyResponse =
        HttpPostJson(server.Port(), "/mcp", unknownKeyRequest);
    Check(unknownKeyResponse.find("unknown argument 'bogus'") !=
              std::string::npos,
          "an unknown argument is rejected and named in the error");
    Check(backend.CurrentDocument().SaveToString() == beforeError,
          "an argument-validation failure leaves the document byte-identical");

    // ---- ID resolution: ambiguous prefix is refused, never guessed ----
    // Every fixture ID shares the "01K3000000000000000000000" prefix, so
    // that exact string is ambiguous among clip/track/source IDs.
    const std::string ambiguousRequest =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K3000000000000000000000","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string ambiguousResponse =
        HttpPostJson(server.Port(), "/mcp", ambiguousRequest);
    Check(ambiguousResponse.find("ambiguous") != std::string::npos,
          "an ambiguous id prefix is refused rather than silently guessed");

    // ---- non-finite number rejected ----
    const std::string hugeExponentRequest =
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":1e400,"rate":25}}}})";
    const std::string hugeExponentResponse =
        HttpPostJson(server.Port(), "/mcp", hugeExponentRequest);
    Check(hugeExponentResponse.find("Parse error") != std::string::npos,
          "a non-finite number literal is rejected");

    // ---- initialize ----
    const std::string initRequest =
        R"({"jsonrpc":"2.0","id":9,"method":"initialize",)"
        R"("params":{"protocolVersion":"2024-11-05","capabilities":{},)"
        R"("clientInfo":{"name":"test","version":"0"}}})";
    const std::string initResponse =
        HttpPostJson(server.Port(), "/mcp", initRequest);
    Check(initResponse.find("\"protocolVersion\":\"2024-11-05\"") !=
              std::string::npos,
          "initialize echoes the negotiated protocol version");
    Check(initResponse.find("\"serverInfo\"") != std::string::npos,
          "initialize reports serverInfo");

    // ---- JSON-RPC notification: no response body over HTTP ----
    const std::string notificationRequest =
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    const std::string notificationResponse =
        HttpPostJson(server.Port(), "/mcp", notificationRequest);
    Check(StatusLine(notificationResponse) == "HTTP/1.1 202 Accepted",
          "a JSON-RPC notification gets HTTP 202 with no JSON-RPC response");
    Check(HttpBody(notificationResponse).empty(),
          "a JSON-RPC notification gets an empty body");

    // ---- unknown method is a JSON-RPC protocol error ----
    const std::string unknownMethodRequest =
        R"({"jsonrpc":"2.0","id":10,"method":"not/a/real/method"})";
    const std::string unknownMethodResponse =
        HttpPostJson(server.Port(), "/mcp", unknownMethodRequest);
    Check(unknownMethodResponse.find("-32601") != std::string::npos,
          "an unknown JSON-RPC method reports 'Method not found'");

    server.Stop();
    Check(!server.IsRunning(), "server reports stopped after Stop()");

    // CA3 -- docs/roadmap/CADRAGE-2026-10.md. track_subject_framing driven
    // by a trajectory handed in, the way a session without Vision drives
    // it: the clip goes to fill, its position gets keys, one undo takes both
    // back; without detections, a backend with no finder refuses.
    {
        Document followed;
        followed.sequence.id = "01K7100000000000000000000S";
        followed.sequence.width = 1080;
        followed.sequence.height = 1920;
        followed.sequence.frame_rate = {25, 1};
        LibraryMedia media;
        media.id = "01K71000000000000000000001";
        media.path = "rushes/A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        followed.library.push_back(media);
        followed.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        DocumentClip clip;
        clip.id = "01K71000000000000000000020";
        clip.source_id = media.id;
        clip.source_in = {0, 25};
        clip.duration = {75, 25};
        clip.timeline_in = {0, 25};
        clip.include_audio = false;
        followed.sequence.tracks = {
            {"01K71000000000000000000010", "video", 0, {clip}}};
        std::string followedError;
        Check(followed.Validate(followedError),
              "tracking fixture validates: " + followedError);

        McpToolRegistry registry;
        InMemoryBackend backend(followed);
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                tool + " arguments parse: " + parseFailure);
            return registry.Call(backend, tool, arguments);
        };
        // A face at 0.3 of the rush, then at 0.7 from frame 20 on.
        std::string samples;
        for (int frame = 0; frame <= 74; frame += 5) {
            const int left = frame < 20 ? 2500 : 6500;
            samples += std::string(samples.empty() ? "" : ",") +
                       R"({"time":{"value":)" + std::to_string(frame) +
                       R"(,"rate":25},"faces":[{"x":{"num":)" +
                       std::to_string(left) +
                       R"(,"den":10000},"y":{"num":3,"den":10},)"
                       R"("width":{"num":1,"den":10},)"
                       R"("height":{"num":1,"den":5}}]})";
        }
        const McpToolCallOutcome tracked =
            call("track_subject_framing",
                 R"({"clip_ids":["01K71000000000000000000020"],"detections":[)"
                 R"({"clip_id":"01K71000000000000000000020","samples":[)" +
                     samples + "]}]}");
        Check(tracked.ok, "track_subject_framing follows a given trajectory: " +
                              tracked.error_name + ' ' + tracked.message);
        Check(tracked.result_json.find("\"faces_from\":\"detections\"") !=
                      std::string::npos &&
                  tracked.result_json.find("\"mode_set_to_fill\":true") !=
                      std::string::npos,
              "and says where the faces came from and that it filled: " +
                  tracked.result_json);
        Document after;
        std::string snapshotMessage;
        backend.SnapshotDocument(after, snapshotMessage);
        const DocumentClip& moved = after.sequence.tracks[0].clips[0];
        Check(moved.framing.mode == FramingMode::Fill &&
                  ClipAnimates(moved, ClipAnimatedParameter::FramingPositionX),
              "the clip fills and its position across carries keys");
        Check(call("undo", "{}").ok, "one undo");
        backend.SnapshotDocument(after, snapshotMessage);
        Check(after.sequence.tracks[0].clips[0].framing.mode ==
                      FramingMode::Fit &&
                  after.sequence.tracks[0].clips[0].animation.empty(),
              "takes the framing and the keys back together");
        Check(!call("track_subject_framing",
                    R"({"clip_ids":["01K71000000000000000000020"]})")
                   .ok,
              "without detections, a backend with no face finder refuses");
        Check(!call("track_subject_framing",
                    R"({"clip_ids":["01K71000000000000000000020"],)"
                    R"("dead_zone":{"num":1,"den":5}})")
                   .ok,
              "a misspelled setting is refused rather than ignored");
        Check(!call("track_subject_framing",
                    R"({"clip_ids":["01K71000000000000000000020"],)"
                    R"("settle":{"num":1,"den":4},"detections":[]})")
                   .ok,
              "a settle wider than the dead zone is refused");

        // PA7 -- docs/roadmap/PARITE-2026-11.md. stabilize_clip on a
        // movement the backend hands over: the clip fills, enlarged by the
        // scale chosen, its position carries keys, one undo takes it back.
        Check(!call("stabilize_clip",
                    R"({"clip_ids":["01K71000000000000000000020"]})")
                   .ok,
              "a backend that cannot read the rush refuses");
        std::vector<StabilizationSample> shake(75);
        for (size_t frame = 1; frame < shake.size(); ++frame) {
            shake[frame].measured = true;
            // Three pixels of a 1920 rush, one way then the other, across
            // and down.
            shake[frame].translation_x = (frame % 2 ? 3.0 : -3.0) / 1920.0;
            shake[frame].translation_y = (frame % 3 ? 2.0 : -4.0) / 1080.0;
        }
        backend.clip_motion["01K71000000000000000000020"] = shake;
        const McpToolCallOutcome steadied = call(
            "stabilize_clip", R"({"clip_ids":["01K71000000000000000000020"]})");
        Check(steadied.ok, "stabilize_clip steadies the clip: " +
                               steadied.error_name + ' ' + steadied.message);
        Check(steadied.result_json.find("\"corrected\":\"translation\"") !=
                      std::string::npos &&
                  steadied.result_json.find("\"scale_chosen\":true") !=
                      std::string::npos &&
                  steadied.result_json.find("\"before_rms_px\"") !=
                      std::string::npos &&
                  steadied.result_json.find("\"frames_unmeasured\":0") !=
                      std::string::npos,
              "and says what it corrected, the scale it chose and the "
              "shake it measured: " +
                  steadied.result_json);
        backend.SnapshotDocument(after, snapshotMessage);
        const DocumentClip& steady = after.sequence.tracks[0].clips[0];
        Check(
            steady.framing.mode == FramingMode::Fill &&
                steady.framing.scale.num > steady.framing.scale.den &&
                ClipAnimates(steady, ClipAnimatedParameter::FramingPositionX) &&
                ClipAnimates(steady, ClipAnimatedParameter::FramingPositionY),
            "the clip fills, enlarged, and both positions carry keys");
        Check(call("undo", "{}").ok, "one undo");
        backend.SnapshotDocument(after, snapshotMessage);
        Check(after.sequence.tracks[0].clips[0].framing.mode ==
                      FramingMode::Fit &&
                  after.sequence.tracks[0].clips[0].animation.empty(),
              "takes the scale and the keys back together");
        Check(!call("stabilize_clip",
                    R"({"clip_ids":["01K71000000000000000000020"],)"
                    R"("smoothing":{"value":0,"rate":1}})")
                      .ok &&
                  !call("stabilize_clip",
                        R"({"clip_ids":["01K71000000000000000000020"],)"
                        R"("strength":1})")
                       .ok,
              "a smoothing of nothing and a setting it does not have are "
              "refused");
    }

    // R1 -- docs/roadmap/REEL-2026-09.md. set_clip_framing puts one framing
    // on several clips in one reversible step, and list_clip_framing is the
    // only way to know what it produced without exporting: guard rail 4,
    // the engine measures.
    {
        Document framed;
        framed.sequence.id = "01K7000000000000000000000S";
        framed.sequence.width = 1080;
        framed.sequence.height = 1920;
        framed.sequence.frame_rate = {25, 1};
        LibraryMedia media;
        media.id = "01K70000000000000000000001";
        media.path = "rushes/A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        framed.library.push_back(media);
        framed.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        DocumentTrack track{"01K70000000000000000000010", "video", 0, {}};
        for (int index = 0; index < 2; ++index) {
            DocumentClip clip;
            clip.id = "01K7000000000000000000002" + std::to_string(index);
            clip.source_id = media.id;
            clip.source_in = {0, 25};
            clip.duration = {10, 25};
            clip.timeline_in = {10 * index, 25};
            clip.include_audio = false;
            track.clips.push_back(clip);
        }
        framed.sequence.tracks = {track};
        std::string framedError;
        Check(framed.Validate(framedError),
              "framing fixture validates: " + framedError);

        McpToolRegistry registry;
        InMemoryBackend backend(framed);
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                tool + " arguments parse: " + parseFailure);
            return registry.Call(backend, tool, arguments);
        };

        const McpToolCallOutcome fitted = call(
            "list_clip_framing", R"({"clip_id":"01K70000000000000000000020"})");
        Check(fitted.ok,
              "list_clip_framing reads an unframed clip: " + fitted.message);
        Check(fitted.result_json.find("\"covers_frame\":false") !=
                      std::string::npos &&
                  fitted.result_json.find("\"top\":656") != std::string::npos,
              "and names the bars a fitted 16:9 rush leaves: " +
                  fitted.result_json);

        const McpToolCallOutcome applied =
            call("set_clip_framing",
                 R"({"clip_ids":["01K70000000000000000000020",)"
                 R"("01K70000000000000000000021"],"mode":"fill",)"
                 R"("scale":{"num":22,"den":20}})");
        Check(applied.ok, "set_clip_framing applies to two clips: " +
                              applied.error_name + ' ' + applied.message);
        Document after;
        std::string snapshotMessage;
        backend.SnapshotDocument(after, snapshotMessage);
        Check(after.sequence.tracks[0].clips[0].framing.mode ==
                      FramingMode::Fill &&
                  after.sequence.tracks[0].clips[1].framing.scale.num == 11 &&
                  after.sequence.tracks[0].clips[1].framing.scale.den == 10,
              "both clips carry the reduced framing");

        const McpToolCallOutcome measured = call(
            "list_clip_framing", R"({"clip_id":"01K70000000000000000000021",)"
                                 R"("offset":{"value":4,"rate":25}})");
        Check(measured.ok,
              "list_clip_framing reads the framed clip: " + measured.message);
        Check(measured.result_json.find("\"covers_frame\":true") !=
                  std::string::npos,
              "which now leaves no bar: " + measured.result_json);
        Check(measured.result_json.find("\"source_frame\":4") !=
                  std::string::npos,
              "and says which source frame it shows: " + measured.result_json);

        Check(!call("set_clip_framing",
                    R"({"clip_ids":["01K70000000000000000000020"],)"
                    R"("crop_left":{"num":1,"den":2},)"
                    R"("crop_right":{"num":1,"den":2}})")
                   .ok,
              "a crop that leaves no picture is refused");
        Check(!call("set_clip_framing", R"({"clip_ids":[]})").ok,
              "an empty clip list is refused");
        Check(!call("set_clip_framing",
                    R"({"clip_ids":["01K70000000000000000000020"],)"
                    R"("mode":"cover"})")
                   .ok,
              "an unknown mode is refused rather than defaulted");
        Check(!call("set_clip_framing",
                    R"({"clip_ids":["01K70000000000000000000020"],)"
                    R"("zoom":{"num":2,"den":1}})")
                   .ok,
              "a misspelled argument is refused rather than ignored");
        // R2 -- docs/roadmap/REEL-2026-09.md. The same clips, animated:
        // set_clip_animation lays one curve on several shots in one step,
        // and list_clip_framing reads back what a given frame is worth --
        // guard rail 4, the engine measures.
        const McpToolCallOutcome animated =
            call("set_clip_animation",
                 R"({"clip_ids":["01K70000000000000000000020",)"
                 R"("01K70000000000000000000021"],"animation":[{)"
                 R"("parameter":"framing.scale","keyframes":[)"
                 R"({"time":{"value":0,"rate":25},"value":{"num":1,"den":1},)"
                 R"("interpolation":"cubic-bezier",)"
                 R"("easing":[{"num":2,"den":5},{"num":0,"den":1},)"
                 R"({"num":1,"den":5},{"num":1,"den":1}]},)"
                 R"({"time":{"value":8,"rate":25},)"
                 R"("value":{"num":23,"den":20}}]}]})");
        Check(animated.ok, "set_clip_animation applies to two clips: " +
                               animated.error_name + ' ' + animated.message);
        Document withKeys;
        backend.SnapshotDocument(withKeys, snapshotMessage);
        Check(withKeys.sequence.tracks[0].clips[0].animation.size() == 1 &&
                  withKeys.sequence.tracks[0].clips[1].animation.size() == 1,
              "both clips carry the track");
        Check(
            withKeys.sequence.tracks[0].clips[0].animation[0].keyframes[0].id !=
                withKeys.sequence.tracks[0]
                    .clips[1]
                    .animation[0]
                    .keyframes[0]
                    .id,
            "with their own keyframe ids, since ids live in one space");
        const McpToolCallOutcome sampled = call(
            "list_clip_framing", R"({"clip_id":"01K70000000000000000000021",)"
                                 R"("offset":{"value":8,"rate":25}})");
        Check(sampled.ok,
              "list_clip_framing reads an animated clip: " + sampled.message);
        Check(sampled.result_json.find(
                  "\"sampled\":{\"scale\":{\"num\":23,\"den\":20}") !=
                  std::string::npos,
              "and reports the value of that frame: " + sampled.result_json);
        Check(!call("set_clip_animation",
                    R"({"clip_ids":["01K70000000000000000000020",)"
                    R"("01K70000000000000000000021"],"animation":[{)"
                    R"("parameter":"framing.scale","keyframes":[)"
                    R"({"id":"01K70000000000000000000K1",)"
                    R"("time":{"value":0,"rate":25},)"
                    R"("value":{"num":1,"den":1}},)"
                    R"({"id":"01K70000000000000000000K2",)"
                    R"("time":{"value":8,"rate":25},)"
                    R"("value":{"num":23,"den":20}}]}]})")
                   .ok,
              "the same explicit keyframe ids on two clips are refused");
        Check(!call("set_clip_animation",
                    R"({"clip_ids":["01K70000000000000000000020"],)"
                    R"("animation":[{"parameter":"framing.rotation",)"
                    R"("keyframes":[]}]})")
                   .ok,
              "an unknown parameter is refused rather than ignored");
        Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok,
              "undo takes the keys back off both clips");
        Document unanimated;
        backend.SnapshotDocument(unanimated, snapshotMessage);
        Check(unanimated.sequence.tracks[0].clips[0].animation.empty() &&
                  unanimated.sequence.tracks[0].clips[1].animation.empty(),
              "and neither clip keeps one");

        Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok,
              "one undo takes both clips back");
        Document undone;
        backend.SnapshotDocument(undone, snapshotMessage);
        Check(undone.SaveToString() == framed.SaveToString(),
              "and the document is byte-identical to the unframed one");
    }

    // SEQ-2026-08 -- conform_sequence. The agent names the intent; the
    // engine computes the pixels, rotation included.
    {
        Document rotated = fixture;
        rotated.sequence.width = 1920;
        rotated.sequence.height = 1080;
        rotated.sequence.frame_rate = {25, 1};
        rotated.library.clear();
        // Document::Validate requires a path and a filename on every library
        // entry, so the fixture carries them: the operation is applied for
        // real, not against a document the engine would reject anyway.
        const auto rush = [](int32_t rateNum, const std::string& name) {
            LibraryMedia media;
            media.id = GenerateUlid();
            media.path = "rushes/" + name;
            media.filename = name;
            media.codec = "h264";
            media.width = 3840;
            media.height = 2160;
            media.rotation_degrees = 90;
            media.orientation = "portrait";
            media.rate = {rateNum, 1};
            media.duration = {100, rateNum};
            return media;
        };
        rotated.library.push_back(rush(25, "A.MP4"));
        rotated.library.push_back(rush(25, "B.MP4"));
        rotated.library.push_back(rush(25, "C.MP4"));
        rotated.library.push_back(rush(50, "D.MP4"));

        McpToolRegistry registry;
        InMemoryBackend backend(rotated);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "conform_sequence arguments parse: " + parseFailure);
            return registry.Call(backend, "conform_sequence", arguments);
        };

        const McpToolCallOutcome preview = call(R"({"preview":true})");
        Check(preview.ok, "conform_sequence previews: " + preview.message);
        Check(preview.result_json.find("\"width\":2160") != std::string::npos &&
                  preview.result_json.find("\"height\":3840") !=
                      std::string::npos,
              "the proposal is the displayed frame, not the stored one: " +
                  preview.result_json);
        Check(
            preview.result_json.find("\"applied\":false") != std::string::npos,
            "a preview reports that it changed nothing");
        Document afterPreview;
        std::string snapshotMessage;
        backend.SnapshotDocument(afterPreview, snapshotMessage);
        Check(afterPreview.sequence.width == 1920,
              "a preview really does leave the sequence alone");
        Check(preview.result_json.find("\"media_count\":3") !=
                      std::string::npos &&
                  preview.result_json.find("\"media_count\":1") !=
                      std::string::npos,
              "both formats are reported with their counts: " +
                  preview.result_json);

        const McpToolCallOutcome applied = call(R"({})");
        Check(applied.ok, "conform_sequence applies: " + applied.error_name +
                              " " + applied.message);
        Check(applied.result_json.find("\"applied\":true") != std::string::npos,
              "applying says so: " + applied.result_json);
        Document conformed;
        backend.SnapshotDocument(conformed, snapshotMessage);
        Check(conformed.sequence.width == 2160 &&
                  conformed.sequence.height == 3840 &&
                  conformed.sequence.frame_rate.num == 25 &&
                  conformed.sequence.frame_rate.den == 1,
              "the sequence now matches the rushes exactly");
        Check(conformed.sequence.name == fixture.sequence.name,
              "conforming the format does not rename the sequence");

        // A second call must not push a no-op onto the undo stack.
        const McpToolCallOutcome again = call(R"({})");
        Check(again.ok &&
                  again.result_json.find("\"already_conformed\":true") !=
                      std::string::npos &&
                  again.result_json.find("\"applied\":false") !=
                      std::string::npos,
              "a sequence already matching is reported, not rewritten: " +
                  again.result_json);
        Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok,
              "one undo is enough to take the format back");
        Document undone;
        backend.SnapshotDocument(undone, snapshotMessage);
        Check(undone.sequence.width == 1920 && undone.sequence.height == 1080,
              "undo restores the previous format exactly");

        Check(!call(R"({"previw":true})").ok,
              "a misspelled argument is refused rather than ignored");

        // A project with nothing to derive a format from must say so.
        Document soundOnly = fixture;
        soundOnly.library.clear();
        InMemoryBackend silent(soundOnly);
        const McpToolCallOutcome nothing = registry.Call(
            silent, "conform_sequence", mcp_json::Value::MakeObject());
        Check(!nothing.ok &&
                  nothing.message.find("picture format") != std::string::npos,
              "an empty library refuses with a reason: " + nothing.message);
    }

    // ALPHA-2026-08 -- transcribe_media. The tool the catalog lacked while
    // every word-level tool in it needed a transcript to exist.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "transcribe_media arguments parse: " + parseFailure);
            return registry.Call(backend, "transcribe_media", arguments);
        };

        const McpToolCallOutcome outcome = call(
            R"({"media_id":"01K30000000000000000000001","language":"fr","verbatim":true})");
        Check(outcome.ok, "transcribe_media succeeds: " + outcome.message);
        Check(backend.transcription_request.seen &&
                  backend.transcription_request.media_ids.size() == 1 &&
                  backend.transcription_request.media_ids[0] ==
                      "01K30000000000000000000001" &&
                  backend.transcription_request.language == "fr" &&
                  backend.transcription_request.verbatim,
              "media, language and verbatim reach the engine unchanged");

        // QC-2026-09 A3 -- the batch form is the whole point of the ticket:
        // one model load for the rushes instead of one per rush.
        backend.transcription_request = {};
        Check(call(R"({"media_ids":["01K30000000000000000000001"]})").ok,
              "a list of media is accepted");
        Check(backend.transcription_request.media_ids.size() == 1,
              "and reaches the engine as a batch");
        Check(!call(R"({"media_id":"01K30000000000000000000001",)"
                    R"("media_ids":["01K30000000000000000000001"]})")
                   .ok,
              "naming both forms is refused rather than resolved arbitrarily");
        backend.transcription_request = {};
        Check(call(R"({"media_id":"01K30000000000000000000001",)"
                   R"("include_silent":true})")
                      .ok &&
                  backend.transcription_request.include_silent,
              "the override for media measured as mute reaches the engine");

        backend.transcription_request = {};
        Check(call(R"({"media_id":"01K30000000000000000000001"})").ok,
              "language and verbatim are optional");
        Check(backend.transcription_request.language == "auto" &&
                  !backend.transcription_request.verbatim &&
                  !backend.transcription_request.overwrite,
              "the defaults are automatic detection, non-verbatim, and a "
              "cache kept from a worse transcription");
        // PRD40 -- the explicit replacement of a cached transcript.
        backend.transcription_request = {};
        Check(call(R"({"media_id":"01K30000000000000000000001",)"
                   R"("overwrite":true})")
                      .ok &&
                  backend.transcription_request.overwrite,
              "overwrite reaches the engine");

        Check(
            !call(
                 R"({"media_id":"01K30000000000000000000001","langauge":"fr"})")
                 .ok,
            "a misspelled argument is refused rather than ignored");
        Check(!call(R"({})").ok, "a media to transcribe is required");
        Check(!call(R"({"media_id":"01K39999999999999999999999"})").ok,
              "an unknown media is refused");

        // The message a user must act on: the model is a local setting, and
        // a failure that does not say so leaves them with nothing to do.
        InMemoryBackend unconfigured(fixture);
        unconfigured.FailTranscription();
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"media_id":"01K30000000000000000000001"})",
                               arguments, parseFailure);
        const McpToolCallOutcome failure =
            registry.Call(unconfigured, "transcribe_media", arguments);
        Check(!failure.ok &&
                  failure.message.find("Whisper model") != std::string::npos,
              "an unconfigured model surfaces as a named reason");
    }

    // B7 -- timeline transcription has its own address and preserves the
    // language/model-mode cache identity inputs through MCP.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        mcp_json::Value arguments;
        std::string parseError;
        Check(
            mcp_json::Value::Parse(
                R"({"timeline_id":"01K30000000000000000000020","language":"fr","verbatim":true})",
                arguments, parseError),
            "transcribe_timeline arguments parse: " + parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "transcribe_timeline", arguments);
        Check(outcome.ok && backend.timeline_transcription_request.seen &&
                  backend.timeline_transcription_request.timeline_id ==
                      "01K30000000000000000000020" &&
                  backend.timeline_transcription_request.language == "fr" &&
                  backend.timeline_transcription_request.verbatim,
              "timeline id, language and verbatim reach timeline "
              "transcription unchanged");

        arguments = mcp_json::Value::MakeObject();
        Check(registry.Call(backend, "transcribe_timeline", arguments).ok &&
                  backend.timeline_transcription_request.timeline_id.empty() &&
                  backend.timeline_transcription_request.language == "auto" &&
                  !backend.timeline_transcription_request.verbatim,
              "timeline transcription defaults to the active timeline and "
              "automatic language detection");
    }

    // QC-2026-09 A4 -- addressing by source frame. The fixture's clip A1
    // plays source frames 100..109 at timeline 5..14, so a caller naming a
    // rush frame must land on the position the engine computes and never on
    // the frame number itself.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                tool + " arguments parse: " + parseFailure);
            return registry.Call(backend, tool, arguments);
        };

        const McpToolCallOutcome located = call(
            "locate_source_frame",
            R"({"media_id":"01K30000000000000000000001","source_frame":105})");
        Check(located.ok, "locate_source_frame succeeds: " + located.message);
        mcp_json::Value view;
        std::string viewError;
        Check(mcp_json::Value::Parse(located.result_json, view, viewError),
              "its result is JSON: " + viewError);
        const mcp_json::Value* matches = view.Find("matches");
        Check(matches != nullptr && matches->IsArray() &&
                  matches->AsArray().size() == 2,
              "the linked video and audio clips play that frame");
        if (matches != nullptr && matches->AsArray().size() == 2) {
            const mcp_json::Value* position =
                matches->AsArray()[0].Find("timeline_position");
            const mcp_json::Value* value =
                position ? position->Find("value") : nullptr;
            int64_t frames = 0;
            Check(value != nullptr && value->AsInt64(frames) && frames == 10,
                  "source frame 105 is at timeline frame 10, not at 105");
        }
        Check(call("locate_source_frame",
                   R"({"media_id":"01K30000000000000000000001",)"
                   R"("source_frame":4000})")
                  .ok,
              "a frame that is not on the timeline is a fact, not an error");

        // The cut takes it too, and lands where locate_source_frame said.
        Check(call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                 R"("source_frame":105})")
                  .ok,
              "split_clip cuts on a source frame");
        const DocumentTrack* track =
            backend.CurrentDocument().FindTrack("01K30000000000000000000002");
        Check(track != nullptr && track->clips.size() == 3,
              "the clip is actually split in two");
        if (track != nullptr && track->clips.size() == 3) {
            Check(track->clips[1].timeline_in == RationalTime{10, 25} &&
                      track->clips[1].source_in == RationalTime{105, 25},
                  "the cut lands on the frame that was named, in both "
                  "domains");
        }
        EditError undoError = EditError::None;
        std::string undoMessage;
        backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                           undoError, undoMessage);

        Check(!call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                  R"("source_frame":105,"timeline_position":)"
                                  R"({"value":10,"rate":25}})")
                   .ok,
              "naming both a source frame and a timeline position is refused "
              "rather than resolved by precedence");
        Check(!call("split_clip", R"({"clip_id":"01K30000000000000000000003"})")
                   .ok,
              "and naming neither is refused too");
        const McpToolCallOutcome outOfBoundsSplit =
            call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                               R"("source_frame":4000})");
        Check(
            !outOfBoundsSplit.ok &&
                outOfBoundsSplit.error_name == "InvalidOperation" &&
                outOfBoundsSplit.message.find("[101, 109]") !=
                    std::string::npos &&
                outOfBoundsSplit.message.find("got 4000") != std::string::npos,
            "a source-frame cut outside the clip is InvalidOperation with "
            "its actual bounds and value: " +
                outOfBoundsSplit.message);

        // A trim reads the same address, with the tail inclusive.
        Check(call("trim_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                R"("edge":"Head","source_frame":103})")
                  .ok,
              "trim_clip enters on a source frame");
        const DocumentClip* trimmed =
            backend.CurrentDocument().FindClip("01K30000000000000000000003");
        Check(trimmed != nullptr && trimmed->source_in == RationalTime{103, 25},
              "and the clip starts on exactly that frame");
        Check(
            !call("trim_clip", R"({"clip_id":"01K30000000000000000000003",)"
                               R"("edge":"Head","delta":{"value":1,"rate":25},)"
                               R"("source_frame":103})")
                 .ok,
            "a delta and a source frame together are refused");
    }

    // AGENT-2026-09 A12 -- lengthening an edge onto frames the neighbouring
    // clip already plays from the same source is said, with the frames and
    // the clip, and left to the caller.
    {
        Document repeat;
        repeat.sequence.id = "01K30000000000000000000905";
        repeat.sources = {
            {"01K30000000000000000000901", "folder/A.MP4", {25, 1}, {1000, 25}},
        };
        repeat.sequence.tracks = {
            {"01K30000000000000000000902",
             "video",
             0,
             {{"01K30000000000000000000903",
               "01K30000000000000000000901",
               {100, 25},
               {10, 25},
               {0, 25}},
              {"01K30000000000000000000904",
               "01K30000000000000000000901",
               {110, 25},
               {10, 25},
               {10, 25}}}},
        };
        McpToolRegistry registry;
        InMemoryBackend backend(repeat);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "ripple_trim arguments parse: " + parseFailure);
            return registry.Call(backend, "ripple_trim", arguments);
        };
        const McpToolCallOutcome tail =
            call(R"({"clip_id":"01K30000000000000000000903","edge":"Tail",)"
                 R"("delta":{"value":5,"rate":25}})");
        mcp_json::Value view;
        std::string parseError;
        Check(tail.ok &&
                  mcp_json::Value::Parse(tail.result_json, view, parseError) &&
                  view.Find("repeats_source") != nullptr &&
                  view.Find("repeats_source")->Find("clip_id")->AsString() ==
                      "01K30000000000000000000904" &&
                  view.Find("repeats_source")->Find("from")->Dump() ==
                      R"({"value":110,"rate":25})" &&
                  view.Find("repeats_source")->Find("to")->Dump() ==
                      R"({"value":115,"rate":25})" &&
                  view.Find("repeats_source")->Find("hint") != nullptr,
              "lengthening a tail onto the next clip's frames names them: " +
                  tail.result_json);
        const McpToolCallOutcome head =
            call(R"({"clip_id":"01K30000000000000000000904","edge":"Head",)"
                 R"("delta":{"value":-3,"rate":25}})");
        Check(head.ok &&
                  mcp_json::Value::Parse(head.result_json, view, parseError) &&
                  view.Find("repeats_source") != nullptr &&
                  view.Find("repeats_source")->Find("clip_id")->AsString() ==
                      "01K30000000000000000000903" &&
                  view.Find("repeats_source")->Find("from")->Dump() ==
                      R"({"value":107,"rate":25})",
              "lengthening a head onto the previous clip's frames names "
              "them: " +
                  head.result_json);
        const McpToolCallOutcome shorter =
            call(R"({"clip_id":"01K30000000000000000000903","edge":"Tail",)"
                 R"("delta":{"value":-2,"rate":25}})");
        Check(
            shorter.ok &&
                mcp_json::Value::Parse(shorter.result_json, view, parseError) &&
                view.Find("repeats_source") == nullptr,
            "shortening repeats nothing and says nothing");
    }

    // QC-2026-09 A2 -- tighten_pauses. What matters at this layer is that the
    // clip's linked sound is carried by the same cut and that the whole thing
    // is one undoable event: the arithmetic itself is pinned in
    // tests/pause_tightening_tests.cc.
    {
        Document pauses;
        pauses.sources = {
            {"01K30000000000000000000001", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture;
        picture.id = "01K30000000000000000000010";
        picture.source_id = "01K30000000000000000000001";
        picture.source_in = {0, 25};
        picture.duration = {75, 25};
        picture.timeline_in = {0, 25};
        picture.link_group_id = "01K30000000000000000000012";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000011";
        pauses.sequence.tracks = {
            {"01K30000000000000000000013", "video", 0, {picture}},
            {"01K30000000000000000000014", "audio", 1, {sound}},
        };

        // Une seconde de parole, une de silence, une de parole.
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000001";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        for (int index = 0; index < 150; ++index)
            envelope.levels.push_back(index >= 50 && index < 100 ? 20000
                                                                 : 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(pauses);
        backend.SetSourceSpeechOnset(envelope);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "tighten_pauses arguments parse: " + parseFailure);
            return registry.Call(backend, "tighten_pauses", arguments);
        };

        const McpToolCallOutcome outcome =
            call(R"({"clip_id":"01K30000000000000000000010"})");
        Check(outcome.ok, "tighten_pauses succeeds: " + outcome.message);
        mcp_json::Value result;
        std::string resultError;
        Check(mcp_json::Value::Parse(outcome.result_json, result, resultError),
              "its result is JSON: " + resultError);
        const mcp_json::Value* applied = result.Find("applied");
        Check(applied != nullptr && applied->IsBool() && applied->AsBool(),
              "a clip with a one-second hole is actually tightened");
        // AGENT-2026-09 A17 -- the flag reaches the engine's settings.
        Check(call(R"({"clip_id":"01K30000000000000000000010",)"
                   R"("close_unheard":true})")
                  .ok,
              "tighten_pauses takes close_unheard");
        // AGENT-2026-09 A26 -- several clips in one call, one result each.
        {
            const McpToolCallOutcome batch =
                call(R"({"clip_ids":["01K30000000000000000000010"],)"
                     R"("close_unheard":true})");
            mcp_json::Value view;
            std::string parseFailure;
            Check(batch.ok &&
                      mcp_json::Value::Parse(batch.result_json, view,
                                             parseFailure) &&
                      view.Find("applied")->Dump() == "1" &&
                      view.Find("results")->AsArray().size() == 1 &&
                      view.Find("results")
                              ->AsArray()[0]
                              .Find("clip_id")
                              ->AsString() == "01K30000000000000000000010" &&
                      view.Find("results")->AsArray()[0].Find("applied") !=
                          nullptr,
                  "clip_ids runs the tool once per clip and lists each "
                  "answer: " +
                      batch.result_json.substr(0, 300));
            CheckFailureEnvelope(
                call(R"({"clip_id":"01K30000000000000000000010",)"
                     R"("clip_ids":["01K30000000000000000000010"]})"),
                "clip_id and clip_ids together", "ValidationFailed");
            CheckFailureEnvelope(call(R"({"clip_ids":[]})"),
                                 "an empty clip_ids", "ValidationFailed");
            CheckFailureEnvelope(
                call(R"({"clip_ids":["01K3000000000000000000FFFF"]})"),
                "a batch over an unknown clip", "ValidationFailed");
        }
        const mcp_json::Value* alsoCut = result.Find("also_cut");
        Check(alsoCut != nullptr && alsoCut->IsArray() &&
                  alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() ==
                      "01K30000000000000000000011",
              "the linked sound is cut by the same operation, not left behind");
        const DocumentTrack* video =
            backend.CurrentDocument().FindTrack("01K30000000000000000000013");
        const DocumentTrack* audio =
            backend.CurrentDocument().FindTrack("01K30000000000000000000014");
        Check(video != nullptr && audio != nullptr &&
                  video->clips.size() == 2 && audio->clips.size() == 2,
              "both tracks are split into the fragments that remain");

        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage),
            "one gesture is one undo step: " + undoMessage);

        // Nothing to close is a success, and says so rather than failing.
        SpeechOnsetReport unbroken;
        unbroken.media_id = "01K30000000000000000000001";
        unbroken.windows_per_second = 50;
        unbroken.decode_sample_rate = 16000;
        unbroken.levels.assign(150, 200000);
        unbroken.speech_level = SpeechLevelPercentile(unbroken.levels, 90);
        unbroken.noise_floor = SpeechLevelPercentile(unbroken.levels, 5);
        InMemoryBackend tight(pauses);
        tight.SetSourceSpeechOnset(unbroken);
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000010"})",
                               arguments, parseFailure);
        const McpToolCallOutcome nothing =
            registry.Call(tight, "tighten_pauses", arguments);
        Check(nothing.ok, "an already tight clip is not an error");
        mcp_json::Value nothingResult;
        mcp_json::Value::Parse(nothing.result_json, nothingResult, resultError);
        const mcp_json::Value* nothingApplied = nothingResult.Find("applied");
        Check(nothingApplied != nullptr && !nothingApplied->AsBool(),
              "and reports that it changed nothing");

        Check(!call(R"({"clip_id":"01K30000000000000000000010","keep":6})").ok,
              "a misspelled argument is refused rather than ignored");

        // IA8 -- the report says whether aligned words guarded the cut. With
        // none cached, the pause closes as before and the words are declared
        // unchecked; with one inside the silence, it survives and is named.
        const mcp_json::Value* unchecked =
            result.Find("report") ? result.Find("report")->Find("words_checked")
                                  : nullptr;
        Check(
            unchecked != nullptr && unchecked->IsBool() && !unchecked->AsBool(),
            "without a transcript the report says words went unchecked");
        InMemoryBackend guarded(pauses);
        guarded.SetSourceSpeechOnset(envelope);
        Transcript aligned;
        aligned.media_id = "01K30000000000000000000001";
        aligned.source_rate = {25, 1};
        aligned.word_timing = "nemo";
        aligned.words = {{"Et", {5, 25}, {20, 25}},
                         {"à", {37, 25}, {38, 25}},
                         {"demain", {52, 25}, {70, 25}}};
        guarded.SetSourceTranscript(aligned);
        const McpToolCallOutcome kept =
            registry.Call(guarded, "tighten_pauses", arguments);
        mcp_json::Value keptResult;
        mcp_json::Value::Parse(kept.result_json, keptResult, resultError);
        const mcp_json::Value* keptReport = keptResult.Find("report");
        const mcp_json::Value* keptWords =
            keptReport ? keptReport->Find("kept_words") : nullptr;
        const mcp_json::Value* timing =
            keptReport ? keptReport->Find("word_timing") : nullptr;
        Check(kept.ok && keptWords != nullptr && keptWords->IsArray() &&
                  keptWords->AsArray().size() == 1 && timing != nullptr &&
                  timing->AsString() == "nemo",
              "an aligned word inside the silence is kept and named: " +
                  kept.result_json);
        const DocumentTrack* guardedAudio =
            guarded.CurrentDocument().FindTrack("01K30000000000000000000014");
        bool wordStillPlayed = false;
        if (guardedAudio != nullptr) {
            for (const DocumentClip& clip : guardedAudio->clips)
                wordStillPlayed =
                    wordStillPlayed ||
                    (clip.source_in <= RationalTime{37, 25} &&
                     RationalTime{38, 25} <= clip.source_in.add(clip.duration));
        }
        Check(wordStillPlayed, "the word is still in the cut sound");

        InMemoryBackend blind(pauses);
        const McpToolCallOutcome missing =
            registry.Call(blind, "tighten_pauses", arguments);
        Check(!missing.ok &&
                  missing.message.find("speech envelope") != std::string::npos,
              "a source with no envelope reports what is missing");
    }

    // B4 -- boundary and junction air are intent-level MCP tools backed by
    // one serialized operation. The pure arithmetic and four-pass regression
    // live in boundary_air_tests; this pins ID parsing, cache access, linked
    // A/V propagation, sync-lock defaults, and the public result envelope.
    {
        Document boundaries;
        boundaries.sources = {
            {"01K30000000000000000000201", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture;
        picture.id = "01K30000000000000000000210";
        picture.source_id = "01K30000000000000000000201";
        picture.source_in = {0, 25};
        picture.duration = {50, 25};
        picture.timeline_in = {0, 25};
        picture.link_group_id = "01K30000000000000000000220";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000211";
        DocumentClip follower = picture;
        follower.id = "01K30000000000000000000212";
        follower.link_group_id.clear();
        follower.source_in = {100, 25};
        follower.duration = {10, 25};
        follower.timeline_in = {50, 25};
        boundaries.sequence.tracks = {
            {"01K30000000000000000000230", "video", 0, {picture}},
            {"01K30000000000000000000231", "audio", 1, {sound}},
            {"01K30000000000000000000232", "video", 2, {follower}},
        };
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000201";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.levels.insert(envelope.levels.end(), 20, 20000);
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.levels.insert(envelope.levels.end(), 30, 20000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(boundaries);
        backend.SetSourceSpeechOnset(envelope);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000210"})",
                               arguments, parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "trim_boundary_air", arguments);
        Check(outcome.ok, "trim_boundary_air succeeds: " + outcome.message);
        mcp_json::Value result;
        Check(mcp_json::Value::Parse(outcome.result_json, result, parseError),
              "trim_boundary_air returns JSON: " + parseError);
        const mcp_json::Value* alsoCut = result.Find("also_cut");
        const mcp_json::Value* syncTracks = result.Find("sync_track_ids");
        Check(alsoCut && alsoCut->IsArray() && alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() == sound.id,
              "trim_boundary_air reports the linked sound it carried");
        Check(syncTracks && syncTracks->IsArray() &&
                  syncTracks->AsArray().size() == 1 &&
                  syncTracks->AsArray()[0].AsString() ==
                      "01K30000000000000000000232",
              "trim_boundary_air derives the other sync-locked track");
        Check(
            backend.CurrentDocument().FindClip(picture.id)->source_in ==
                    RationalTime{7, 25} &&
                backend.CurrentDocument().FindClip(sound.id)->source_in ==
                    RationalTime{7, 25} &&
                backend.CurrentDocument().FindClip(follower.id)->timeline_in ==
                    RationalTime{31, 25},
            "MCP reaches the atomic engine operation on every track");
    }

    {
        Document junction;
        junction.sources = {
            {"01K30000000000000000000301", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip left;
        left.id = "01K30000000000000000000310";
        left.source_id = "01K30000000000000000000301";
        left.source_in = {0, 25};
        left.duration = {32, 25};
        left.timeline_in = {0, 25};
        DocumentClip right = left;
        right.id = "01K30000000000000000000311";
        right.source_in = {32, 25};
        right.timeline_in = {32, 25};
        junction.sequence.tracks = {
            {"01K30000000000000000000320", "audio", 0, {left, right}},
        };
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000301";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.levels.insert(envelope.levels.end(), 28, 20000);
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(junction);
        backend.SetSourceSpeechOnset(envelope);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(
            R"({"left_clip_id":"01K30000000000000000000310","right_clip_id":"01K30000000000000000000311","keep_frames":2})",
            arguments, parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "close_junction_air", arguments);
        Check(outcome.ok, "close_junction_air succeeds: " + outcome.message);
        const DocumentClip* newLeft =
            backend.CurrentDocument().FindClip(left.id);
        const DocumentClip* newRight =
            backend.CurrentDocument().FindClip(right.id);
        Check(newLeft && newRight &&
                  newLeft->duration == RationalTime{27, 25} &&
                  newRight->source_in == RationalTime{37, 25} &&
                  newRight->timeline_in == RationalTime{27, 25},
              "close_junction_air trims both sides in its MCP call");
    }

    // IA7 -- ROADMAP.md. A server started with no project answers only the
    // two calls that give it one; every other tool is refused by name, and
    // the session calls reach the backend without a snapshot to resolve
    // against. The locking itself is pinned in mcp_tests on the real backend.
    {
        class ClosedBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool HasOpenProject() const override { return open; }
            bool RunAfterRequest(const std::string& requestJson,
                                 std::string& resultJson, std::string&,
                                 std::string&) override {
                after_requests.push_back(requestJson);
                resultJson = R"({"ok":true,"project":{"path":""}})";
                return true;
            }
            std::vector<std::string> after_requests;
            bool SnapshotDocument(Document& document,
                                  std::string& message) override {
                ++snapshots;
                return InMemoryBackend::SnapshotDocument(document, message);
            }
            bool CreateProject(const std::string& path, const std::string& name,
                               const std::string& format,
                               std::string& resultJson, std::string&,
                               std::string&) override {
                created = path + "|" + name + "|" + format;
                open = true;
                resultJson = R"({"path":"created"})";
                return true;
            }
            bool open = false;
            int snapshots = 0;
            std::string created;
        };
        ClosedBackend closed(fixture);
        McpToolRegistry registry;
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                tool + " arguments parse: " + parseFailure);
            return registry.Call(closed, tool, arguments);
        };
        const McpToolCallOutcome refused = call("describe", "{}");
        Check(!refused.ok && refused.error_name == "NoProjectOpen" &&
                  refused.message.find("create_project") != std::string::npos,
              "with no project open, a tool is refused and told what to call");
        // MOTION-AFTER-2026-09 MA10 -- the After Effects tools address an
        // .aep, never the CUTMACHINE project: they reach the bridge with no
        // project open, without a snapshot.
        {
            const McpToolCallOutcome described =
                call("after_describe_project", "{}");
            Check(described.ok && closed.after_requests.size() == 1 &&
                      closed.after_requests.front().find("describe_project") !=
                          std::string::npos &&
                      closed.snapshots == 0,
                  "after_describe_project runs with no project open: " +
                      described.error_name + " " + described.message);
            const McpToolCallOutcome unknown =
                call("after_list_comps", R"({"comp":"x"})");
            Check(!unknown.ok && unknown.error_name != "NoProjectOpen" &&
                      closed.after_requests.size() == 1,
                  "an After tool still checks its arguments: " +
                      unknown.error_name);
        }
        // PRD18 -- ROADMAP.md (ACP4). The procedures are read before there
        // is a project, from the binary: no snapshot, no backend call.
        {
            const McpToolCallOutcome listed = call("get_procedure", "{}");
            mcp_json::Value listing;
            std::string listingError;
            const mcp_json::Value* procedures =
                listed.ok && mcp_json::Value::Parse(listed.result_json, listing,
                                                    listingError)
                    ? listing.Find("procedures")
                    : nullptr;
            Check(procedures != nullptr && procedures->IsArray() &&
                      procedures->AsArray().size() == ListProcedures().size() &&
                      closed.snapshots == 0,
                  "get_procedure lists the procedures with no project open: " +
                      listed.result_json.substr(0, 200));
            ProcedureText skill;
            std::string errorName;
            std::string message;
            ReadProcedureFile("monter-une-itw", "SKILL.md", skill, errorName,
                              message);
            const McpToolCallOutcome read =
                call("get_procedure", R"({"name":"monter-une-itw"})");
            mcp_json::Value answer;
            Check(read.ok &&
                      mcp_json::Value::Parse(read.result_json, answer,
                                             listingError) &&
                      answer.Find("file") != nullptr &&
                      answer.Find("file")->AsString() == "SKILL.md" &&
                      answer.Find("text") != nullptr &&
                      answer.Find("text")->AsString() == skill.text &&
                      !skill.text.empty() && answer.Find("files") != nullptr,
                  "get_procedure with a name gives its SKILL.md and its files");
            const McpToolCallOutcome cited =
                call("get_procedure",
                     R"({"name":"monter-une-itw","file":"mots-et-bornes.md"})");
            Check(cited.ok && cited.result_json.find(
                                  "\"file\":\"mots-et-bornes.md\"") !=
                                  std::string::npos,
                  "get_procedure with a file gives that file");
            const McpToolCallOutcome unknown =
                call("get_procedure", R"({"name":"monter-un-film"})");
            Check(
                !unknown.ok && unknown.error_name == "UnknownProcedure" &&
                    unknown.message.find("monter-une-itw") != std::string::npos,
                "an unknown procedure is refused with the known ones: " +
                    unknown.message);
            const McpToolCallOutcome fileAlone =
                call("get_procedure", R"({"file":"SKILL.md"})");
            Check(
                !fileAlone.ok && fileAlone.error_name == "ValidationFailed" &&
                    fileAlone.message.find("give 'name'") != std::string::npos,
                "a file without its procedure is refused: " +
                    fileAlone.message);
            const McpToolCallOutcome escaping = call(
                "get_procedure",
                R"({"name":"monter-un-reel","file":"../monter-une-itw/resolve.md"})");
            Check(!escaping.ok && escaping.error_name == "ValidationFailed" &&
                      escaping.message.find("procedure 'monter-une-itw'") !=
                          std::string::npos,
                  "a path out of the procedure is refused with the call to "
                  "make: " +
                      escaping.message);
            Check(!call("get_procedure", R"({"name":"x","page":2})").ok,
                  "get_procedure refuses an unknown argument");
            Check(closed.snapshots == 0 && !closed.open,
                  "no procedure read touched the backend");
        }
        Check(!call("ingest_media", R"({"path":"/tmp/rushes"})").ok,
              "ingesting needs an open project too");
        Check(!call("create_project", R"({"path":"/tmp/x","title":"X"})").ok,
              "create_project refuses an unknown argument");
        const McpToolCallOutcome created = call(
            "create_project",
            R"({"path":"/tmp/ITW.cutmachine-project","name":"ITW","format":"1080x1920@25"})");
        Check(created.ok &&
                  closed.created ==
                      "/tmp/ITW.cutmachine-project|ITW|1080x1920@25" &&
                  closed.snapshots == 0,
              "create_project reaches the backend without a snapshot: " +
                  created.message);
        Check(call("describe", "{}").ok,
              "once a project is open, the catalog answers");

        InMemoryBackend bound(fixture);
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"path":"/tmp/Other.cutmachine-project"})",
                               arguments, parseFailure);
        const McpToolCallOutcome unsupported =
            registry.Call(bound, "open_project", arguments);
        Check(
            !unsupported.ok && unsupported.error_name == "UnsupportedOperation",
            "a backend bound to one document cannot open another");
    }

    // IA9 -- ROADMAP.md. Every tool that cuts sound names the aligned words
    // it took, split between the words asked for and the words carried off.
    // The classification itself is pinned in cut_words_tests; this pins that
    // each tool measures what it is about to apply and says so.
    {
        const std::string rush = "01K30000000000000000000401";
        const std::string picture = "01K30000000000000000000410";
        const std::string sound = "01K30000000000000000000411";
        Document cuts;
        cuts.sources = {{rush, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip pictureClip;
        pictureClip.id = picture;
        pictureClip.source_id = rush;
        pictureClip.source_in = {0, 25};
        pictureClip.duration = {100, 25};
        pictureClip.timeline_in = {0, 25};
        pictureClip.link_group_id = "01K30000000000000000000420";
        DocumentClip soundClip = pictureClip;
        soundClip.id = sound;
        cuts.sequence.tracks = {
            {"01K30000000000000000000430", "video", 0, {pictureClip}},
            {"01K30000000000000000000431", "audio", 1, {soundClip}},
        };
        // Silence up to frame 10, speech to 45, a hollow to 60, speech to 90,
        // silence to 100. The « euh » at the head and the « et » in the
        // hollow are quiet enough to read as silence.
        SpeechOnsetReport envelope;
        envelope.media_id = rush;
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.levels.insert(envelope.levels.end(), 20, 20000);
        envelope.levels.insert(envelope.levels.end(), 70, 200000);
        envelope.levels.insert(envelope.levels.end(), 30, 20000);
        envelope.levels.insert(envelope.levels.end(), 60, 200000);
        envelope.levels.insert(envelope.levels.end(), 20, 20000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);
        Transcript aligned;
        aligned.media_id = rush;
        aligned.source_rate = {25, 1};
        aligned.verbatim = true;
        aligned.word_timing = "nemo";
        aligned.words = {
            {"euh", {3, 25}, {8, 25}},    {"Alors", {12, 25}, {20, 25}},
            {"à", {30, 25}, {31, 25}},    {"demain", {32, 25}, {44, 25}},
            {"et", {50, 25}, {51, 25}},   {"matin", {62, 25}, {80, 25}},
            {"voilà", {82, 25}, {89, 25}}};

        McpToolRegistry registry;
        // "à demain~": the words of one list, "~" marking a word cut into.
        const auto texts = [](const mcp_json::Value* list) {
            std::string text;
            if (list == nullptr || !list->IsArray()) return std::string("?");
            for (const mcp_json::Value& word : list->AsArray()) {
                if (!text.empty()) text += ' ';
                const mcp_json::Value* whole = word.Find("whole");
                text += word.Find("text")->AsString() +
                        (whole != nullptr && whole->AsBool() ? "" : "~");
            }
            return text;
        };
        struct CutWordsSeen {
            bool present = false;
            bool checked = false;
            std::string targeted;
            std::string carried;
            std::string json;
        };
        const auto run = [&](const Document& document, bool withWords,
                             const std::string& tool,
                             const std::string& argumentsJson) {
            InMemoryBackend backend(document);
            backend.SetSourceSpeechOnset(envelope);
            if (withWords) backend.SetSourceTranscript(aligned);
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            const McpToolCallOutcome outcome =
                registry.Call(backend, tool, arguments);
            Check(outcome.ok, tool + " succeeds: " + outcome.message);
            CutWordsSeen seen;
            seen.json = outcome.result_json;
            mcp_json::Value result;
            if (!mcp_json::Value::Parse(outcome.result_json, result,
                                        parseError))
                return seen;
            const mcp_json::Value* report = result.Find("cut_words");
            if (report == nullptr) return seen;
            seen.present = true;
            seen.checked = report->Find("checked")->AsBool();
            const mcp_json::Value* sources = report->Find("sources");
            if (sources != nullptr && sources->AsArray().size() == 1) {
                seen.targeted = texts(sources->AsArray()[0].Find("targeted"));
                seen.carried = texts(sources->AsArray()[0].Find("carried"));
            }
            return seen;
        };

        CutWordsSeen seen = run(
            cuts, true, "remove_words",
            R"({"clip_id":"01K30000000000000000000411","ranges":[{"start_word_index":2,"end_word_index":2}]})");
        Check(seen.checked && seen.targeted == "à" && seen.carried.empty(),
              "remove_words names the word it was asked for as targeted: " +
                  seen.json);
        // AGENT-2026-09 A10 -- with the envelope in cache, the cut was
        // placed on it, and says so.
        Check(
            seen.json.find("\"boundaries\":\"envelope\"") != std::string::npos,
            "remove_words says its cut was placed on the envelope: " +
                seen.json);

        seen = run(cuts, true, "clean_disfluencies",
                   R"({"clip_id":"01K30000000000000000000411"})");
        Check(seen.checked && seen.targeted == "euh" && seen.carried.empty(),
              "clean_disfluencies names the hesitation as targeted: " +
                  seen.json);

        seen = run(
            cuts, true, "remove_source_ranges",
            R"({"clip_id":"01K30000000000000000000411","ranges":[{"first_frame":30,"end_frame":36}]})");
        Check(seen.checked && seen.targeted.empty() &&
                  seen.carried == "à demain~",
              "remove_source_ranges names a whole word and one cut into: " +
                  seen.json);

        seen = run(
            cuts, true, "ripple_trim",
            R"({"clip_id":"01K30000000000000000000411","edge":"Head","delta":{"value":15,"rate":25}})");
        Check(seen.checked && seen.carried == "euh Alors~",
              "ripple_trim names what its head trim took: " + seen.json);

        seen = run(cuts, true, "trim_boundary_air",
                   R"({"clip_id":"01K30000000000000000000411"})");
        Check(seen.checked && seen.carried == "euh~",
              "trim_boundary_air names the quiet word its trim cut into: " +
                  seen.json);

        seen = run(
            cuts, true, "tighten_pauses",
            R"({"clip_id":"01K30000000000000000000411","min_gap_ms":150,"keep_frames":2})");
        Check(seen.present && seen.checked && seen.targeted.empty() &&
                  seen.carried.empty(),
              "a cut in the silence names no word, and says it checked: " +
                  seen.json);

        seen = run(
            cuts, false, "remove_source_ranges",
            R"({"clip_id":"01K30000000000000000000411","ranges":[{"first_frame":30,"end_frame":36}]})");
        Check(seen.present && !seen.checked &&
                  seen.json.find("\"reason\":") != std::string::npos &&
                  seen.json.find("\"first_frame\":30,\"end_frame\":36") !=
                      std::string::npos,
              "without aligned words the cut is declared unchecked, with its "
              "frames: " +
                  seen.json);

        // The junction case: an « et » straddling the splice goes with both
        // trims.
        Document junction;
        junction.sources = cuts.sources;
        DocumentClip left = soundClip;
        left.link_group_id.clear();
        left.duration = {32, 25};
        DocumentClip right = left;
        right.id = "01K30000000000000000000412";
        right.source_in = {32, 25};
        right.timeline_in = {32, 25};
        junction.sequence.tracks = {
            {"01K30000000000000000000431", "audio", 0, {left, right}}};
        SpeechOnsetReport splice;
        splice.media_id = rush;
        splice.windows_per_second = 50;
        splice.decode_sample_rate = 16000;
        splice.levels.insert(splice.levels.end(), 50, 200000);
        splice.levels.insert(splice.levels.end(), 28, 20000);
        splice.levels.insert(splice.levels.end(), 50, 200000);
        splice.speech_level = SpeechLevelPercentile(splice.levels, 90);
        splice.noise_floor = SpeechLevelPercentile(splice.levels, 5);
        Transcript spliceWords = aligned;
        spliceWords.words = {{"mais", {20, 25}, {25, 25}},
                             {"et", {31, 25}, {33, 25}},
                             {"bon", {39, 25}, {45, 25}}};
        InMemoryBackend junctionBackend(junction);
        junctionBackend.SetSourceSpeechOnset(splice);
        junctionBackend.SetSourceTranscript(spliceWords);
        mcp_json::Value junctionArguments;
        std::string junctionError;
        mcp_json::Value::Parse(
            R"({"left_clip_id":"01K30000000000000000000411","right_clip_id":"01K30000000000000000000412","keep_frames":2})",
            junctionArguments, junctionError);
        const McpToolCallOutcome closed = registry.Call(
            junctionBackend, "close_junction_air", junctionArguments);
        mcp_json::Value closedResult;
        mcp_json::Value::Parse(closed.result_json, closedResult, junctionError);
        const mcp_json::Value* closedWords = closedResult.Find("cut_words");
        const mcp_json::Value* closedSources =
            closedWords ? closedWords->Find("sources") : nullptr;
        Check(closed.ok && closedSources != nullptr &&
                  closedSources->AsArray().size() == 1 &&
                  texts(closedSources->AsArray()[0].Find("carried")) == "et",
              "close_junction_air names the word straddling the splice: " +
                  closed.result_json);
    }

    // BENCH-2026-09 -- after remove_source_ranges every fragment of the pair
    // keeps one link id. trim_boundary_air and close_junction_air carried the
    // whole group: on a real run a 5-frame head trim on one fragment trimmed
    // the 17 others, left 5-frame holes, cut into words, and answered
    // applied: true. Only the fragment, its facing sound, and the words they
    // lose may move; cut_words names exactly those words.
    {
        const Ulid rush = "01K30000000000000000000501";
        const Ulid videoTrack = "01K30000000000000000000510";
        const Ulid audioTrack = "01K30000000000000000000511";
        Document block;
        block.sources = {{rush, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture{
            "01K30000000000000000000512", rush, {0, 25}, {200, 25}, {0, 25}};
        picture.link_group_id = "01K30000000000000000000514";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000513";
        block.sequence.tracks = {{videoTrack, "video", 0, {picture}},
                                 {audioTrack, "audio", 1, {sound}}};
        // Source frames: speech 0-40, room 40-60, speech 60-100, room
        // 100-120, speech 120-160, room 160-180, speech 180-200.
        SpeechOnsetReport envelope;
        envelope.media_id = rush;
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        for (int phrase = 0; phrase < 3; ++phrase) {
            envelope.levels.insert(envelope.levels.end(), 80, 200000);
            envelope.levels.insert(envelope.levels.end(), 40, 20000);
        }
        envelope.levels.insert(envelope.levels.end(), 40, 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);
        // One aligned word at the head of each fragment: only the one inside
        // the trimmed air may be reported.
        Transcript aligned;
        aligned.media_id = rush;
        aligned.source_rate = {25, 1};
        aligned.verbatim = true;
        aligned.word_timing = "nemo";
        aligned.words = {{"Alors", {1, 25}, {6, 25}},
                         {"euh", {53, 25}, {56, 25}},
                         {"mais", {113, 25}, {116, 25}},
                         {"donc", {173, 25}, {176, 25}}};

        McpToolRegistry registry;
        InMemoryBackend backend(block);
        backend.SetSourceSpeechOnset(envelope);
        backend.SetSourceTranscript(aligned);
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson,
                              mcp_json::Value& result) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            const McpToolCallOutcome outcome =
                registry.Call(backend, tool, arguments);
            Check(outcome.ok, tool + " succeeds: " + outcome.message);
            mcp_json::Value::Parse(outcome.result_json, result, parseError);
            return outcome.ok;
        };
        const auto fragmentAt =
            [&](const Ulid& trackId,
                int64_t sourceFrame) -> const DocumentClip* {
            const DocumentTrack* track =
                backend.CurrentDocument().FindTrack(trackId);
            if (track == nullptr) return nullptr;
            for (const DocumentClip& clip : track->clips)
                if (clip.source_in == RationalTime{sourceFrame, 25})
                    return &clip;
            return nullptr;
        };
        const auto plays = [&](int64_t sourceIn, int64_t duration,
                               int64_t timelineIn) {
            bool both = true;
            for (const Ulid& trackId : {videoTrack, audioTrack}) {
                const DocumentClip* clip = fragmentAt(trackId, sourceIn);
                both = both && clip != nullptr &&
                       clip->duration == RationalTime{duration, 25} &&
                       clip->timeline_in == RationalTime{timelineIn, 25};
            }
            return both;
        };
        const auto contiguous = [&]() {
            for (const Ulid& trackId : {videoTrack, audioTrack}) {
                std::vector<DocumentClip> clips =
                    backend.CurrentDocument().FindTrack(trackId)->clips;
                std::sort(
                    clips.begin(), clips.end(),
                    [](const DocumentClip& left, const DocumentClip& right) {
                        return left.timeline_in < right.timeline_in;
                    });
                RationalTime end{0, 25};
                for (const DocumentClip& clip : clips) {
                    if (clip.timeline_in != end) return false;
                    end = clip.timeline_in.add(clip.duration);
                }
            }
            return true;
        };
        const auto ids = [](const mcp_json::Value& result,
                            const std::string& key) {
            std::vector<std::string> list;
            const mcp_json::Value* array = result.Find(key);
            if (array != nullptr && array->IsArray())
                for (const mcp_json::Value& item : array->AsArray())
                    list.push_back(item.AsString());
            return list;
        };
        const auto carried = [](const mcp_json::Value& result) {
            const mcp_json::Value* report = result.Find("cut_words");
            const mcp_json::Value* sources =
                report != nullptr ? report->Find("sources") : nullptr;
            if (sources == nullptr || sources->AsArray().size() != 1)
                return std::string("?");
            std::string text;
            const mcp_json::Value& source = sources->AsArray()[0];
            for (const char* key : {"targeted", "carried"}) {
                const mcp_json::Value* words = source.Find(key);
                if (words == nullptr) return std::string("?");
                for (const mcp_json::Value& word : words->AsArray()) {
                    text += std::string(text.empty() ? "" : " ") + key[0] +
                            ':' + word.Find("text")->AsString();
                }
            }
            return text;
        };

        mcp_json::Value result;
        call("remove_source_ranges",
             R"({"clip_id":"01K30000000000000000000512","ranges":[)"
             R"({"first_frame":44,"end_frame":52},)"
             R"({"first_frame":104,"end_frame":112},)"
             R"({"first_frame":164,"end_frame":172}]})",
             result);
        const DocumentClip* second = fragmentAt(videoTrack, 52);
        const DocumentClip* secondSound = fragmentAt(audioTrack, 52);
        const DocumentClip* third = fragmentAt(videoTrack, 112);
        const DocumentClip* thirdSound = fragmentAt(audioTrack, 112);
        // Out of lot, 29 October 2026 -- each kept stretch is a pair of its
        // own, as a cut in the window leaves it; the four used to share the
        // block's group.
        Check(
            second != nullptr && secondSound != nullptr && third != nullptr &&
                thirdSound != nullptr &&
                second->link_group_id == secondSound->link_group_id &&
                third->link_group_id == thirdSound->link_group_id &&
                second->link_group_id != picture.link_group_id &&
                third->link_group_id != second->link_group_id &&
                backend.CurrentDocument().FindTrack(videoTrack)->clips.size() ==
                    4,
            "remove_source_ranges leaves four fragments, each in its own pair");
        if (second != nullptr && secondSound != nullptr && third != nullptr &&
            thirdSound != nullptr) {
            const Ulid secondId = second->id;
            const Ulid secondSoundId = secondSound->id;
            const Ulid thirdId = third->id;
            const Ulid thirdSoundId = thirdSound->id;
            const std::string fragmented =
                backend.CurrentDocument().SaveToString();

            call("trim_boundary_air", R"({"clip_id":")" + secondId + R"("})",
                 result);
            Check(ids(result, "also_cut") ==
                      std::vector<std::string>{secondSoundId},
                  "trim_boundary_air carries the facing sound only: " +
                      result.Dump());
            Check(plays(0, 44, 0) && plays(57, 47, 44) && plays(112, 52, 91) &&
                      plays(172, 28, 143),
                  "only the trimmed fragment loses its head; the others move "
                  "up by 5 frames");
            Check(contiguous(),
                  "no gap appears on the picture or the sound track");
            Check(carried(result) == "c:euh",
                  "cut_words names the word in the trimmed air and no other: " +
                      result.Dump());
            call("undo", "{}", result);
            Check(backend.CurrentDocument().SaveToString() == fragmented,
                  "undo restores the fragmented block byte for byte");

            call("close_junction_air",
                 R"({"left_clip_id":")" + secondId + R"(","right_clip_id":")" +
                     thirdId + R"("})",
                 result);
            Check(ids(result, "also_cut") ==
                      std::vector<std::string>{secondSoundId, thirdSoundId},
                  "close_junction_air carries each side's own sound only: " +
                      result.Dump());
            Check(plays(0, 44, 0) && plays(52, 51, 44) && plays(117, 47, 95) &&
                      plays(172, 28, 142),
                  "only the two sides of the junction move");
            Check(contiguous(), "closing the junction leaves no gap");
            Check(carried(result) == "c:mais",
                  "cut_words names the word at the incoming head only: " +
                      result.Dump());
            call("undo", "{}", result);
            Check(backend.CurrentDocument().SaveToString() == fragmented,
                  "undo of the junction restores the bytes");
        }
    }

    // BENCH-2026-09 -- roll_edit moves the boundary between two cartons in
    // one call. It used to answer UnknownSource, a caption having no source,
    // and the agent moved the boundary with two trim_clip calls.
    {
        Document captions = fixture;
        CaptionStyle style;
        style.id = "01K30000000000000000000601";
        captions.sequence.caption_styles = {style};
        DocumentClip first;
        first.id = "01K30000000000000000000602";
        first.duration = {20, 25};
        first.timeline_in = {0, 25};
        first.include_audio = false;
        first.caption_group_id = style.id;
        first.caption_text = "Bonjour";
        DocumentClip second = first;
        second.id = "01K30000000000000000000603";
        second.timeline_in = {20, 25};
        second.duration = {10, 25};
        second.caption_text = "à tous";
        captions.sequence.tracks.push_back(
            {"01K30000000000000000000604", "caption", 2, {first, second}});
        McpToolRegistry registry;
        InMemoryBackend backend(captions);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto roll = [&](const std::string& left, const std::string& right,
                              int64_t frames) {
            mcp_json::Value arguments;
            std::string parseError;
            mcp_json::Value::Parse(R"({"pairs":[{"left_clip_id":")" + left +
                                       R"(","right_clip_id":")" + right +
                                       R"("}],"delta":{"value":)" +
                                       std::to_string(frames) +
                                       R"(,"rate":25}})",
                                   arguments, parseError);
            return registry.Call(backend, "roll_edit", arguments);
        };
        const McpToolCallOutcome rolled = roll(first.id, second.id, 3);
        const DocumentClip* rolledFirst =
            backend.CurrentDocument().FindClip(first.id);
        const DocumentClip* rolledSecond =
            backend.CurrentDocument().FindClip(second.id);
        Check(rolled.ok && rolledFirst != nullptr && rolledSecond != nullptr &&
                  rolledFirst->duration == RationalTime{23, 25} &&
                  rolledSecond->timeline_in == RationalTime{23, 25} &&
                  rolledSecond->duration == RationalTime{7, 25} &&
                  rolledFirst->caption_text == "Bonjour" &&
                  rolledSecond->caption_text == "à tous",
              "roll_edit moves a carton boundary and keeps each text: " +
                  rolled.result_json);
        mcp_json::Value undoArguments = mcp_json::Value::MakeObject();
        Check(registry.Call(backend, "undo", undoArguments).ok &&
                  backend.CurrentDocument().SaveToString() == before,
              "one undo restores both cartons byte for byte");

        const McpToolCallOutcome emptied = roll(first.id, second.id, 10);
        Check(
            !emptied.ok && emptied.error_name == "InvalidOperation" &&
                emptied.result_json.find("\"ok\":false") != std::string::npos &&
                emptied.message.find(second.id) != std::string::npos,
            "a roll that empties a carton is refused by name: " +
                emptied.result_json);
        const McpToolCallOutcome mixed =
            roll(first.id, "01K30000000000000000000003", 1);
        Check(
            !mixed.ok && mixed.error_name == "InvalidOperation" &&
                mixed.message.find("cannot pair caption clip") !=
                    std::string::npos,
            "a carton never rolls against a media clip: " + mixed.result_json);
        Check(backend.CurrentDocument().SaveToString() == before,
              "refused caption rolls leave the document intact");
    }

    // FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. slide_edit and rate_stretch
    // reach their operations, undo in one step, and pass a refusal through
    // by name.
    {
        Document shots;
        shots.sources = {
            {"01K30000000000000000000701", "A.MP4", {25, 1}, {1000, 25}}};
        const auto shot = [](const char* id, int64_t sourceIn, int64_t in) {
            DocumentClip clip;
            clip.id = id;
            clip.source_id = "01K30000000000000000000701";
            clip.source_in = {sourceIn, 25};
            clip.duration = {10, 25};
            clip.timeline_in = {in, 25};
            return clip;
        };
        shots.sequence.tracks = {
            {"01K30000000000000000000702",
             "video",
             0,
             {shot("01K30000000000000000000703", 100, 0),
              shot("01K30000000000000000000704", 200, 10),
              shot("01K30000000000000000000705", 300, 20),
              shot("01K30000000000000000000706", 400, 40)}}};
        McpToolRegistry registry;
        InMemoryBackend backend(shots);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const mcp_json::Value undoArguments = mcp_json::Value::MakeObject();

        const McpToolCallOutcome slid =
            call("slide_edit", R"({"clip_ids":["01K30000000000000000000704"],)"
                               R"("delta":{"value":2,"rate":25}})");
        const DocumentClip* middle =
            backend.CurrentDocument().FindClip("01K30000000000000000000704");
        Check(slid.ok && middle != nullptr &&
                  middle->timeline_in == RationalTime{12, 25} &&
                  middle->source_in == RationalTime{200, 25},
              "slide_edit moves the clip between its neighbours: " +
                  slid.result_json);
        Check(slid.result_json.find("\"speed\"") == std::string::npos,
              "a delta leaves out a speed of 1/1 nobody changed: " +
                  slid.result_json);
        Check(registry.Call(backend, "undo", undoArguments).ok &&
                  backend.CurrentDocument().SaveToString() == before,
              "one undo restores the slide byte for byte");
        const McpToolCallOutcome gap =
            call("slide_edit", R"({"clip_ids":["01K30000000000000000000705"],)"
                               R"("delta":{"value":1,"rate":25}})");
        Check(!gap.ok && gap.error_name == "InvalidOperation" &&
                  gap.message.find("move_clip") != std::string::npos,
              "slide_edit refuses a clip with a gap beside it: " +
                  gap.result_json);

        const McpToolCallOutcome stretched =
            call("rate_stretch", R"({"clip_id":"01K30000000000000000000705",)"
                                 R"("duration":{"value":20,"rate":25}})");
        const DocumentClip* third =
            backend.CurrentDocument().FindClip("01K30000000000000000000705");
        Check(stretched.ok && third != nullptr && third->speed.num == 1 &&
                  third->speed.den == 2 &&
                  third->duration == RationalTime{20, 25} &&
                  third->timeline_in == RationalTime{20, 25},
              "rate_stretch gives the clip 1/2 by default from its tail: " +
                  stretched.result_json);
        // A caller that patches its model from the delta learns the speed,
        // and learns it again when the undo sets it back to 1/1.
        Check(stretched.result_json.find(R"("speed":{"num":1,"den":2})") !=
                  std::string::npos,
              "the stretch's delta carries the new speed: " +
                  stretched.result_json);
        const McpToolCallOutcome unstretched =
            registry.Call(backend, "undo", undoArguments);
        Check(unstretched.ok &&
                  backend.CurrentDocument().SaveToString() == before,
              "one undo restores the stretch byte for byte");
        Check(unstretched.result_json.find(R"("speed":{"num":1,"den":1})") !=
                  std::string::npos,
              "the undo's delta says the speed is 1/1 again: " +
                  unstretched.result_json);
        const McpToolCallOutcome headed =
            call("rate_stretch",
                 R"({"clip_id":"01K30000000000000000000706","edge":"Head",)"
                 R"("duration":{"value":20,"rate":25}})");
        const DocumentClip* last =
            backend.CurrentDocument().FindClip("01K30000000000000000000706");
        Check(headed.ok && last != nullptr &&
                  last->timeline_in == RationalTime{30, 25},
              "rate_stretch with edge Head keeps the tail: " +
                  headed.result_json);
        const McpToolCallOutcome inexact =
            call("rate_stretch", R"({"clip_id":"01K30000000000000000000703",)"
                                 R"("duration":{"value":1001,"rate":25}})");
        Check(!inexact.ok && inexact.error_name == "InvalidOperation" &&
                  inexact.message.find("1000") != std::string::npos,
              "rate_stretch refuses a speed with no exact spelling: " +
                  inexact.result_json);
        const McpToolCallOutcome unknown = call(
            "rate_stretch", R"({"clip_id":"01K30000000000000000000705",)"
                            R"("duration":{"value":20,"rate":25},"speed":1})");
        Check(!unknown.ok, "rate_stretch refuses an unknown argument");
    }

    // FILMCRAFT-2026-10, review of 18 October 2026 -- slide_edit takes the
    // linked sound with it unless linked_clip_ids says otherwise, as
    // ripple_trim does: a picture slid alone left its sound behind.
    {
        Document pair;
        pair.sources = {
            {"01K30000000000000000000801", "A.MP4", {25, 1}, {1000, 25}}};
        const auto shot = [](const char* id, int64_t sourceIn, int64_t in,
                             const char* group) {
            DocumentClip clip;
            clip.id = id;
            clip.source_id = "01K30000000000000000000801";
            clip.source_in = {sourceIn, 25};
            clip.duration = {10, 25};
            clip.timeline_in = {in, 25};
            clip.link_group_id = group;
            return clip;
        };
        const char* const kGroupA = "01K30000000000000000000811";
        const char* const kGroupB = "01K30000000000000000000812";
        const char* const kGroupC = "01K30000000000000000000813";
        pair.sequence.tracks = {
            {"01K30000000000000000000802",
             "video",
             0,
             {shot("01K30000000000000000000803", 100, 0, kGroupA),
              shot("01K30000000000000000000804", 200, 10, kGroupB),
              shot("01K30000000000000000000805", 300, 20, kGroupC)}},
            {"01K30000000000000000000806",
             "audio",
             1,
             {shot("01K30000000000000000000807", 100, 0, kGroupA),
              shot("01K30000000000000000000808", 200, 10, kGroupB),
              shot("01K30000000000000000000809", 300, 20, kGroupC)}}};
        McpToolRegistry registry;
        InMemoryBackend backend(pair);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
                  "slide_edit arguments parse: " + parseError);
            return registry.Call(backend, "slide_edit", arguments);
        };
        const auto at = [&](const char* id) {
            const DocumentClip* clip = backend.CurrentDocument().FindClip(id);
            return clip != nullptr ? clip->timeline_in : RationalTime{-1, 1};
        };
        const McpToolCallOutcome together =
            call(R"({"clip_ids":["01K30000000000000000000804"],)"
                 R"("delta":{"value":2,"rate":25}})");
        Check(together.ok &&
                  at("01K30000000000000000000804") == RationalTime{12, 25} &&
                  at("01K30000000000000000000808") == RationalTime{12, 25},
              "the picture slides and its sound slides with it: " +
                  together.result_json);
        Check(
            registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok &&
                backend.CurrentDocument().SaveToString() == before,
            "one undo puts both back");
        const McpToolCallOutcome alone =
            call(R"({"clip_ids":["01K30000000000000000000804"],)"
                 R"("delta":{"value":2,"rate":25},"linked_clip_ids":[]})");
        Check(alone.ok &&
                  at("01K30000000000000000000804") == RationalTime{12, 25} &&
                  at("01K30000000000000000000808") == RationalTime{10, 25},
              "linked_clip_ids [] slides the picture alone, as asked: " +
                  alone.result_json);
    }

    // DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. slip_edit takes its
    // linked partner along by default, as slide_edit does, and [] keeps the
    // recalage of one track; rate_stretch ripples when asked, its sync
    // tracks chosen as ripple_trim chooses them.
    {
        Document pair;
        pair.sources = {
            {"01K30000000000000000000901", "A.MP4", {25, 1}, {1000, 25}}};
        const auto shot = [](const char* id, int64_t sourceIn, int64_t in,
                             const char* group) {
            DocumentClip clip;
            clip.id = id;
            clip.source_id = "01K30000000000000000000901";
            clip.source_in = {sourceIn, 25};
            clip.duration = {10, 25};
            clip.timeline_in = {in, 25};
            clip.link_group_id = group;
            return clip;
        };
        const char* const kGroupA = "01K30000000000000000000911";
        const char* const kGroupB = "01K30000000000000000000912";
        const char* const kPicture = "01K30000000000000000000904";
        const char* const kSound = "01K30000000000000000000908";
        DocumentTrack freeTrack{
            "01K30000000000000000000920",
            "video",
            2,
            {shot("01K30000000000000000000921", 500, 30, "")}};
        freeTrack.sync_lock = false;
        pair.sequence.tracks = {
            {"01K30000000000000000000902",
             "video",
             0,
             {shot("01K30000000000000000000903", 100, 0, kGroupA),
              shot(kPicture, 200, 10, kGroupB),
              shot("01K30000000000000000000905", 300, 20, "")}},
            {"01K30000000000000000000906",
             "audio",
             1,
             {shot("01K30000000000000000000907", 100, 0, kGroupA),
              shot(kSound, 200, 10, kGroupB)}},
            freeTrack,
            {"01K30000000000000000000930",
             "video",
             3,
             {shot("01K30000000000000000000931", 600, 25, "")}}};
        std::string validation;
        Check(pair.Validate(validation), "DEC40 fixture: " + validation);
        McpToolRegistry registry;
        InMemoryBackend backend(pair);
        const std::string before = backend.CurrentDocument().SaveToString();
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseError;
            Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
                  tool + " arguments parse: " + parseError);
            return registry.Call(backend, tool, arguments);
        };
        const auto clip = [&](const char* id) {
            return backend.CurrentDocument().FindClip(id);
        };
        const auto undo = [&] {
            return registry.Call(backend, "undo", mcp_json::Value::MakeObject())
                       .ok &&
                   backend.CurrentDocument().SaveToString() == before;
        };

        const McpToolCallOutcome slipped =
            call("slip_edit", R"({"clip_ids":["01K30000000000000000000904"],)"
                              R"("delta":{"value":2,"rate":25}})");
        Check(slipped.ok &&
                  clip(kPicture)->source_in == RationalTime{202, 25} &&
                  clip(kSound)->source_in == RationalTime{202, 25} &&
                  clip(kPicture)->timeline_in == RationalTime{10, 25},
              "slip_edit slips the picture and its sound together: " +
                  slipped.result_json);
        Check(undo(), "one undo puts both back");
        const McpToolCallOutcome alone =
            call("slip_edit",
                 R"({"clip_ids":["01K30000000000000000000904"],)"
                 R"("delta":{"value":2,"rate":25},"linked_clip_ids":[]})");
        Check(alone.ok && clip(kPicture)->source_in == RationalTime{202, 25} &&
                  clip(kSound)->source_in == RationalTime{200, 25},
              "linked_clip_ids [] slips the picture alone, to re-sync it: " +
                  alone.result_json);
        Check(undo(), "and one undo puts it back");

        const McpToolCallOutcome rippled =
            call("rate_stretch",
                 R"({"clip_id":"01K30000000000000000000904","ripple":true,)"
                 R"("duration":{"value":20,"rate":25}})");
        Check(rippled.ok && clip(kPicture)->duration == RationalTime{20, 25} &&
                  clip(kSound)->duration == RationalTime{20, 25} &&
                  clip("01K30000000000000000000905")->timeline_in ==
                      RationalTime{30, 25},
              "ripple: the group stretches and the next clip moves: " +
                  rippled.result_json);
        Check(clip("01K30000000000000000000931")->timeline_in ==
                      RationalTime{35, 25} &&
                  clip("01K30000000000000000000921")->timeline_in ==
                      RationalTime{30, 25},
              "a sync-locked track follows by default, one whose lock is off "
              "does not");
        Check(rippled.result_json.find("warning_track_ids") !=
                      std::string::npos &&
                  rippled.result_json.find("01K30000000000000000000920") !=
                      std::string::npos,
              "and that one is named in a warning: " + rippled.result_json);
        Check(undo(), "one undo restores the ripple byte for byte");
        const McpToolCallOutcome own =
            call("rate_stretch",
                 R"({"clip_id":"01K30000000000000000000904","ripple":true,)"
                 R"("duration":{"value":20,"rate":25},"sync_track_ids":[]})");
        Check(own.ok &&
                  clip("01K30000000000000000000931")->timeline_in ==
                      RationalTime{25, 25} &&
                  own.result_json.find("warning") == std::string::npos,
              "sync_track_ids [] moves only the group's own tracks: " +
                  own.result_json);
        Check(undo(), "undone");
        const McpToolCallOutcome stray =
            call("rate_stretch",
                 R"({"clip_id":"01K30000000000000000000904",)"
                 R"("duration":{"value":20,"rate":25},)"
                 R"("sync_track_ids":["01K30000000000000000000930"]})");
        Check(!stray.ok && stray.message.find("ripple") != std::string::npos &&
                  backend.CurrentDocument().SaveToString() == before,
              "sync_track_ids without ripple is refused, nothing written: " +
                  stray.result_json);
        const McpToolCallOutcome blocked =
            call("rate_stretch", R"({"clip_id":"01K30000000000000000000904",)"
                                 R"("duration":{"value":20,"rate":25}})");
        Check(!blocked.ok && blocked.error_name == "Overlap",
              "without ripple the stretch into the next clip is still an "
              "overlap: " +
                  blocked.result_json);
    }

    // QC-2026-09 (A1) -- align_transcript. The correction only reaches the
    // tools that cut on words when `apply` does, so what this pins is that
    // the flag survives the dispatcher rather than defaulting either way by
    // accident.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "align_transcript arguments parse: " + parseFailure);
            return registry.Call(backend, "align_transcript", arguments);
        };

        Check(call(R"({})").ok, "align_transcript takes no required argument");
        Check(
            backend.alignment_request.seen && !backend.alignment_request.apply,
            "reading what the signal contradicts writes nothing by default");
        Check(call(R"({"apply":true})").ok, "the write form succeeds");
        Check(backend.alignment_request.apply,
              "apply reaches the engine rather than being swallowed");
        Check(!call(R"({"aply":true})").ok,
              "a misspelled argument is refused rather than ignored");

        class BareBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool AlignSourceTranscripts(bool, std::string&,
                                        std::string& message) override {
                return McpBackend::AlignSourceTranscripts(false, message,
                                                          message);
            }
        };
        BareBackend bare(fixture);
        mcp_json::Value empty = mcp_json::Value::MakeObject();
        const McpToolCallOutcome missing =
            registry.Call(bare, "align_transcript", empty);
        Check(!missing.ok &&
                  missing.message.find("transcript cache") != std::string::npos,
              "a backend without a transcript cache reports the gap");

        // AX2 -- align_words checks its arguments before the backend runs
        // anything, and a backend without a project says so.
        mcp_json::Value badTexts;
        std::string parseError;
        mcp_json::Value::Parse(R"({"texts":["pas un objet"]})", badTexts,
                               parseError);
        const McpToolCallOutcome refusedTexts =
            registry.Call(bare, "align_words", badTexts);
        Check(!refusedTexts.ok &&
                  refusedTexts.message.find("texts") != std::string::npos,
              "texts must be an object: " + refusedTexts.message);
        const McpToolCallOutcome noProject =
            registry.Call(bare, "align_words", empty);
        Check(!noProject.ok &&
                  noProject.message.find("align words") != std::string::npos,
              "a backend without a project cannot align words: " +
                  noProject.message);

        // PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. NeMo found nowhere is
        // MfaNotFound over MCP as on the command line: the dispatcher only
        // relays the backend's sentence, which used to read as
        // ValidationFailed.
        class NoMfaBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool AlignSourceWords(const std::vector<Ulid>&, const std::string&,
                                  std::string&, std::string& message) override {
                message =
                    "MFA not found: MFA's Python is at none of "
                    "'/repo/build/mfa/env/bin/python'. Set "
                    "CUTMACHINE_MFA_PYTHON to MFA's Python, in the "
                    "environment";
                return false;
            }
        };
        NoMfaBackend noMfa(fixture);
        const McpToolCallOutcome notFound =
            registry.Call(noMfa, "align_words", empty);
        Check(!notFound.ok && notFound.error_name == "MfaNotFound" &&
                  notFound.result_json.find("\"error\":\"MfaNotFound\"") !=
                      std::string::npos &&
                  notFound.result_json.find("CUTMACHINE_MFA_PYTHON") !=
                      std::string::npos,
              "a missing MFA is refused by name over MCP: " +
                  notFound.result_json);
    }

    // AX5 -- docs/roadmap/AX-2026-09.md. get_editor_view names what the
    // editor points at, resolved against the timeline on screen; a backend
    // with no window says it has none.
    {
        class WindowBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool ReadEditorView(EditorView& view, std::string&) override {
                view.timeline_id = "01K3000000000000000000000W";
                view.playhead = RationalTime{8, 25};
                view.selected_clip_ids = {"01K30000000000000000000004"};
                return true;
            }
        };
        McpToolRegistry registry;
        const auto text = [](const mcp_json::Value& object, const char* key) {
            const mcp_json::Value* found = object.Find(key);
            return found != nullptr && found->IsString() ? found->AsString()
                                                         : std::string();
        };
        WindowBackend window(Fixture());
        const McpToolCallOutcome seen = registry.Call(
            window, "get_editor_view", mcp_json::Value::MakeObject());
        mcp_json::Value view;
        std::string parseError;
        mcp_json::Value::Parse(seen.result_json, view, parseError);
        const mcp_json::Value* under = view.Find("under_playhead");
        const mcp_json::Value* selected = view.Find("selected");
        Check(seen.ok && view.Find("playhead_frame") != nullptr &&
                  view.Find("playhead_frame")->Dump() == "8",
              "the playhead as a sequence frame: " + seen.result_json);
        Check(under != nullptr && under->AsArray().size() == 2 &&
                  text(under->AsArray()[0], "clip_id") ==
                      "01K30000000000000000000003" &&
                  under->AsArray()[0].Find("source_frame")->Dump() == "103",
              "the clips under the playhead, one per track, with the source "
              "frame each plays there: " +
                  seen.result_json);
        Check(selected != nullptr && selected->AsArray().size() == 1 &&
                  text(selected->AsArray()[0], "clip_id") ==
                      "01K30000000000000000000004",
              "the selection, resolved to its clips: " + seen.result_json);
        // D5, D6 -- docs/roadmap/DIALOGUE-2026-10.md. The passage the
        // editor marked around the playhead is named, a point elsewhere is
        // not.
        Check(
            view.Find("markers_under_playhead") != nullptr &&
                view.Find("markers_under_playhead")->AsArray().empty(),
            "no marker under the playhead, an empty list: " + seen.result_json);
        mcp_json::Value spanArguments;
        mcp_json::Value::Parse(
            R"({"name":"ce passage","time":{"value":5,"rate":25},)"
            R"("duration":{"value":10,"rate":25}})",
            spanArguments, parseError);
        mcp_json::Value pointArguments;
        mcp_json::Value::Parse(
            R"({"name":"ailleurs","time":{"value":40,"rate":25}})",
            pointArguments, parseError);
        Check(registry.Call(window, "add_marker", spanArguments).ok &&
                  registry.Call(window, "add_marker", pointArguments).ok,
              "a span and a point are added");
        const McpToolCallOutcome inSpan = registry.Call(
            window, "get_editor_view", mcp_json::Value::MakeObject());
        mcp_json::Value spanView;
        mcp_json::Value::Parse(inSpan.result_json, spanView, parseError);
        const mcp_json::Value* marked = spanView.Find("markers_under_playhead");
        Check(inSpan.ok && marked != nullptr && marked->AsArray().size() == 1 &&
                  text(marked->AsArray()[0], "name") == "ce passage" &&
                  marked->AsArray()[0].Find("time_frame")->Dump() == "5" &&
                  marked->AsArray()[0].Find("duration_frames")->Dump() == "10",
              "the playhead inside a span names it, with its start and "
              "length in frames: " +
                  inSpan.result_json);
        // D5 -- the In/Out range is absent until marked, then named exactly
        // and in frames, with whether the window would act on it.
        Check(spanView.Find("marked_range") == nullptr,
              "no In or Out set, no marked_range: " + inSpan.result_json);
        class MarkedBackend : public WindowBackend {
        public:
            using WindowBackend::WindowBackend;
            std::optional<RationalTime> in;
            std::optional<RationalTime> out;
            bool ReadEditorView(EditorView& view,
                                std::string& message) override {
                WindowBackend::ReadEditorView(view, message);
                view.mark_in = in;
                view.mark_out = out;
                return true;
            }
        };
        MarkedBackend markedWindow(Fixture());
        markedWindow.in = RationalTime{10, 25};
        markedWindow.out = RationalTime{30, 25};
        const McpToolCallOutcome ranged = registry.Call(
            markedWindow, "get_editor_view", mcp_json::Value::MakeObject());
        mcp_json::Value rangedView;
        mcp_json::Value::Parse(ranged.result_json, rangedView, parseError);
        Check(ranged.ok && rangedView.Find("marked_range") != nullptr &&
                  rangedView.Find("marked_range")->Dump() ==
                      R"({"in":{"value":10,"rate":25},"in_frame":10,)"
                      R"("out":{"value":30,"rate":25},"out_frame":30,)"
                      R"("valid":true})",
              "In and Out, exact and in frames, valid: " + ranged.result_json);
        markedWindow.in.reset();
        const McpToolCallOutcome outOnly = registry.Call(
            markedWindow, "get_editor_view", mcp_json::Value::MakeObject());
        mcp_json::Value outOnlyView;
        mcp_json::Value::Parse(outOnly.result_json, outOnlyView, parseError);
        Check(outOnly.ok && outOnlyView.Find("marked_range") != nullptr &&
                  outOnlyView.Find("marked_range")->Dump() ==
                      R"({"out":{"value":30,"rate":25},"out_frame":30,)"
                      R"("valid":false})",
              "an Out alone is said, and not valid: " + outOnly.result_json);
        InMemoryBackend headless(Fixture());
        const McpToolCallOutcome refused = registry.Call(
            headless, "get_editor_view", mcp_json::Value::MakeObject());
        Check(!refused.ok &&
                  refused.message.find("editor window") != std::string::npos,
              "a backend with no window has no view: " + refused.message);
    }

    // CF1 -- the real live-backend seam and tool registry. The host owns
    // display state; choosing an absent/the editor's screen must not alter
    // the previous choice, the document, or the edit journal.
    {
        Document document = Fixture();
        EditLog edits;
        const std::string before = document.SaveToString();
        McpLiveBackend live(document, edits);
        VideoOutputState state{{{"editor", "Built-in", 3024, 1964, true},
                                {"external", "External", 3840, 2160, false}},
                               "",
                               ""};
        live.SetVideoOutputCallback(
            [&](const std::optional<std::string>& screen, VideoOutputState& out,
                std::string& errorName, std::string& message) {
                if (screen) {
                    if (!ValidateVideoOutputScreen(*screen, state, message)) {
                        errorName = "ValidationFailed";
                        return false;
                    }
                    state.selected_screen_id = *screen;
                    state.active_screen_id = *screen;
                }
                out = state;
                return true;
            });
        McpToolRegistry registry;
        const auto call = [&](const char* tool, const char* args) {
            mcp_json::Value value;
            std::string error;
            Check(mcp_json::Value::Parse(args, value, error), "CF1: test JSON");
            return registry.Call(live, tool, value);
        };
        const auto read = call("get_video_output", "{}");
        Check(read.ok && read.result_json.find("3840") != std::string::npos &&
                  read.result_json.find("rec709_sdr") != std::string::npos &&
                  state.selected_screen_id.empty(),
              "CF1: reads the available displays without selecting one");
        Check(call("set_video_output", R"({"screen_id":"external"})").ok &&
                  state.active_screen_id == "external",
              "CF1: selects the external display through the live backend");
        for (const char* bad :
             {R"({"screen_id":"editor"})", R"({"screen_id":"disconnected"})",
              "{}", R"({"screen_id":12})",
              R"({"screen_id":"","unexpected":true})"}) {
            Check(!call("set_video_output", bad).ok &&
                      state.active_screen_id == "external",
                  "CF1: rejects an invalid destination without losing output");
        }
        state.screens.pop_back();
        state.active_screen_id.clear();
        const auto unplugged = call("get_video_output", "{}");
        Check(unplugged.ok && state.selected_screen_id == "external" &&
                  unplugged.result_json.find("\"enabled\":false") !=
                      std::string::npos,
              "CF1: a disconnected output retains its choice, and says off");
        Check(call("set_video_output", R"({"screen_id":""})").ok &&
                  state.selected_screen_id.empty(),
              "CF1: disabling also clears a disconnected choice");
        Check(document.SaveToString() == before && edits.AppliedCount() == 0,
              "CF1: display changes leave project bytes and journal intact");
        InMemoryBackend headless(Fixture());
        CheckFailureEnvelope(registry.Call(headless, "get_video_output",
                                           mcp_json::Value::MakeObject()),
                             "CF1: headless video output",
                             "UnsupportedOperation");
    }

    // VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. capture_editor_window
    // hands the window's picture over as an MCP image, with where its Metal
    // surfaces sit; what it asks the window is what the caller asked; a
    // backend with no window says it has none.
    {
        class PaintedBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            EditorCaptureRequest asked;
            bool CaptureEditorWindow(const EditorCaptureRequest& request,
                                     EditorCapture& capture, std::string&,
                                     std::string&) override {
                asked = request;
                capture.jpeg = "fake-jpeg";
                capture.width = 1512;
                capture.height = 982;
                capture.regions = {{"timeline", 200, 560, 1100, 380},
                                   {"record", 760, 40, 560, 470}};
                return true;
            }
        };
        McpToolRegistry registry;
        PaintedBackend painted(Fixture());
        const McpToolCallOutcome whole = registry.Call(
            painted, "capture_editor_window", mcp_json::Value::MakeObject());
        Check(whole.ok && whole.image_mime == "image/jpeg" &&
                  whole.image_base64 == "ZmFrZS1qcGVn" &&
                  painted.asked.region == "window" && painted.asked.scale == 1,
              "the whole window at a pixel per point, as an image: " +
                  whole.message);
        Check(whole.result_json ==
                  R"({"region":"window","scale":1,"width":1512,)"
                  R"("height":982,"regions":[{"name":"timeline","x":200,)"
                  R"("y":560,"width":1100,"height":380},{"name":"record",)"
                  R"("x":760,"y":40,"width":560,"height":470}]})",
              "the text says the size and where the surfaces sit: " +
                  whole.result_json);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(R"({"region":"Record","scale":2})", arguments,
                               parseError);
        const McpToolCallOutcome record =
            registry.Call(painted, "capture_editor_window", arguments);
        Check(record.ok && painted.asked.region == "record" &&
                  painted.asked.scale == 2,
              "a region in any case, at the backing scale: " + record.message);
        for (const char* wrong :
             {R"({"region":"inspector"})", R"({"scale":3})", R"({"scale":1.5})",
              R"({"path":"/tmp/x.png"})"}) {
            mcp_json::Value bad;
            mcp_json::Value::Parse(wrong, bad, parseError);
            const McpToolCallOutcome refusedArgument =
                registry.Call(painted, "capture_editor_window", bad);
            Check(!refusedArgument.ok &&
                      refusedArgument.error_name == "ValidationFailed",
                  std::string("refused by name: ") + wrong + " -> " +
                      refusedArgument.message);
        }
        InMemoryBackend headless(Fixture());
        const McpToolCallOutcome refused = registry.Call(
            headless, "capture_editor_window", mcp_json::Value::MakeObject());
        Check(!refused.ok && refused.error_name == "UnsupportedOperation" &&
                  refused.message.find("editor window") != std::string::npos,
              "a backend with no window has no picture: " + refused.message);
    }

    // A backend that never implements it must say so rather than pretend.
    {
        class BareBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool TranscribeSources(const std::vector<Ulid>&, const std::string&,
                                   bool, bool, bool, std::string&,
                                   std::string& message) override {
                return McpBackend::TranscribeSources(
                    {}, std::string(), false, false, false, message, message);
            }
        };
        McpToolRegistry registry;
        BareBackend bare(fixture);
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"media_id":"01K30000000000000000000001"})",
                               arguments, parseFailure);
        const McpToolCallOutcome outcome =
            registry.Call(bare, "transcribe_media", arguments);
        Check(
            !outcome.ok && outcome.message.find("cannot run a transcription") !=
                               std::string::npos,
            "a backend without transcription reports the gap explicitly");
    }

    // QC-2026-09 B5 -- ripple_trim carries the linked picture/sound pair by
    // default, while an explicit linked_clip_ids array is an intentional
    // subset. A1 is deliberately left outside the default set so the tool
    // can warn that an omitted sync_track_ids would break V1/V2/A1 alignment.
    {
        const Ulid sourceId = "01K30000000000000000000001";
        const Ulid groupId = "01K30000000000000000000010";
        Document ripple;
        ripple.sources = {{sourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip v1{
            "01K30000000000000000000011", sourceId, {0, 25}, {10, 25}, {0, 25}};
        v1.link_group_id = groupId;
        v1.sync_anchor_clip_id = v1.id;
        DocumentClip v2 = v1;
        v2.id = "01K30000000000000000000012";
        v2.sync_anchor_clip_id = v1.id;
        DocumentClip v1After{"01K30000000000000000000013",
                             sourceId,
                             {20, 25},
                             {10, 25},
                             {10, 25}};
        DocumentClip v2After = v1After;
        v2After.id = "01K30000000000000000000014";
        DocumentClip a1{"01K30000000000000000000015",
                        sourceId,
                        {40, 25},
                        {10, 25},
                        {10, 25}};
        const Ulid v1TrackId = "01K30000000000000000000016";
        const Ulid v2TrackId = "01K30000000000000000000017";
        const Ulid a1TrackId = "01K30000000000000000000018";
        ripple.sequence.tracks = {
            {v1TrackId, "video", 0, {v1, v1After}},
            {v2TrackId, "video", 1, {v2, v2After}},
            {a1TrackId, "audio", 2, {a1}},
        };

        McpToolRegistry registry;
        InMemoryBackend backend(ripple);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "ripple_trim arguments parse: " + parseFailure);
            return registry.Call(backend, "ripple_trim", arguments);
        };

        const std::string before = backend.CurrentDocument().SaveToString();
        const McpToolCallOutcome carried =
            call(R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                 R"("delta":{"value":5,"rate":25}})");
        Check(carried.ok, "ripple_trim carries the linked group by default: " +
                              carried.message);
        const Document& carriedDocument = backend.CurrentDocument();
        Check(
            carriedDocument.FindClip("01K30000000000000000000011")->duration ==
                    RationalTime{15, 25} &&
                carriedDocument.FindClip("01K30000000000000000000012")
                        ->duration == RationalTime{15, 25},
            "default ripple trims both V1 and V2");
        Check(carriedDocument.FindClip("01K30000000000000000000013")
                          ->timeline_in == RationalTime{15, 25} &&
                  carriedDocument.FindClip("01K30000000000000000000014")
                          ->timeline_in == RationalTime{15, 25},
              "default ripple keeps downstream V1/V2 aligned");
        // AGENT-2026-09 A34 -- without sync_track_ids, every other
        // sync-locked track follows, as the Document's sync_lock promises
        // and as every other ripple tool does.
        Check(carriedDocument.FindClip("01K30000000000000000000015")
                      ->timeline_in == RationalTime{15, 25},
              "unlisted, sync-locked A1 follows the ripple by default");
        mcp_json::Value carriedResult;
        std::string resultError;
        Check(mcp_json::Value::Parse(carried.result_json, carriedResult,
                                     resultError),
              "ripple_trim result is JSON: " + resultError);
        const mcp_json::Value* alsoCut = carriedResult.Find("also_cut");
        Check(alsoCut && alsoCut->IsArray() && alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() == v2.id,
              "ripple_trim reports the linked partner in also_cut");
        Check(carriedResult.Find("warning") == nullptr,
              "a track that follows is nothing to warn about");
        {
            Document unlocked = ripple;
            unlocked.sequence.tracks[2].sync_lock = false;
            InMemoryBackend unlockedBackend(unlocked);
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(
                    R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                    R"("delta":{"value":5,"rate":25}})",
                    arguments, parseFailure),
                "ripple_trim arguments parse: " + parseFailure);
            const McpToolCallOutcome left =
                registry.Call(unlockedBackend, "ripple_trim", arguments);
            mcp_json::Value leftResult;
            Check(left.ok &&
                      unlockedBackend.CurrentDocument()
                              .FindClip("01K30000000000000000000015")
                              ->timeline_in == RationalTime{10, 25} &&
                      mcp_json::Value::Parse(left.result_json, leftResult,
                                             resultError) &&
                      leftResult.Find("warning") != nullptr &&
                      leftResult.Find("warning_track_ids") != nullptr,
                  "a track whose sync lock is off stays, and the warning "
                  "names it: " +
                      left.result_json.substr(0, 200));
        }
        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage) &&
                backend.CurrentDocument().SaveToString() == before,
            "default ripple undo restores V1/V2/A1 byte-identically");

        InMemoryBackend subset(ripple);
        const McpToolCallOutcome explicitSubset = [&] {
            mcp_json::Value arguments;
            std::string parseFailure;
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                R"("delta":{"value":5,"rate":25},"linked_clip_ids":[]})",
                arguments, parseFailure);
            return registry.Call(subset, "ripple_trim", arguments);
        }();
        Check(explicitSubset.ok &&
                  subset.CurrentDocument()
                          .FindClip("01K30000000000000000000011")
                          ->duration == RationalTime{15, 25} &&
                  subset.CurrentDocument()
                          .FindClip("01K30000000000000000000012")
                          ->duration == RationalTime{10, 25},
              "explicit empty linked_clip_ids preserves a subset");
        mcp_json::Value subsetResult;
        Check(mcp_json::Value::Parse(explicitSubset.result_json, subsetResult,
                                     resultError) &&
                  subsetResult.Find("also_cut") &&
                  subsetResult.Find("also_cut")->AsArray().empty(),
              "explicit subset reports no linked partner cut");

        InMemoryBackend aligned(ripple);
        mcp_json::Value alignedArguments;
        std::string alignedParseError;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                  R"("delta":{"value":5,"rate":25},)"
                  R"("sync_track_ids":["01K30000000000000000000018"]})",
                  alignedArguments, alignedParseError),
              "aligned ripple arguments parse: " + alignedParseError);
        const McpToolCallOutcome alignment =
            registry.Call(aligned, "ripple_trim", alignedArguments);
        Check(alignment.ok && aligned.CurrentDocument()
                                      .FindClip("01K30000000000000000000015")
                                      ->timeline_in == RationalTime{15, 25},
              "explicit sync_track_ids keeps V1/V2/A1 aligned");
        mcp_json::Value alignmentResult;
        Check(mcp_json::Value::Parse(alignment.result_json, alignmentResult,
                                     resultError) &&
                  alignmentResult.Find("warning") == nullptr,
              "explicit synchronization suppresses the omission warning");
    }

    // ITW7 -- a pair cut into fragments keeps one link id. The default
    // partner of an edge is the facing fragment on the other track, so the
    // first head and the last tail of a timeline can be lengthened.
    {
        const Ulid sourceId = "01K30000000000000000000001";
        const Ulid groupId = "01K30000000000000000000030";
        Document fragments;
        fragments.sources = {{sourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        const auto piece = [&](const Ulid& id, int64_t sourceIn,
                               int64_t timelineIn) {
            DocumentClip clip{
                id, sourceId, {sourceIn, 25}, {10, 25}, {timelineIn, 25}};
            clip.link_group_id = groupId;
            clip.sync_anchor_clip_id = "01K30000000000000000000031";
            return clip;
        };
        fragments.sequence.tracks = {
            {"01K30000000000000000000035",
             "video",
             0,
             {piece("01K30000000000000000000031", 20, 0),
              piece("01K30000000000000000000032", 50, 10)}},
            {"01K30000000000000000000036",
             "audio",
             1,
             {piece("01K30000000000000000000033", 20, 0),
              piece("01K30000000000000000000034", 50, 10)}},
        };
        McpToolRegistry registry;
        InMemoryBackend backend(fragments);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            mcp_json::Value::Parse(argumentsJson, arguments, parseFailure);
            return registry.Call(backend, "ripple_trim", arguments);
        };
        const McpToolCallOutcome tail =
            call(R"({"clip_id":"01K30000000000000000000032","edge":"Tail",)"
                 R"("delta":{"value":5,"rate":25}})");
        const Document& afterTail = backend.CurrentDocument();
        Check(tail.ok &&
                  afterTail.FindClip("01K30000000000000000000032")->duration ==
                      RationalTime{15, 25} &&
                  afterTail.FindClip("01K30000000000000000000034")->duration ==
                      RationalTime{15, 25} &&
                  afterTail.FindClip("01K30000000000000000000031")->duration ==
                      RationalTime{10, 25} &&
                  afterTail.FindClip("01K30000000000000000000033")->duration ==
                      RationalTime{10, 25},
              "the last tail of a fragmented pair lengthens with its facing "
              "partner only: " +
                  tail.message);
        const McpToolCallOutcome head =
            call(R"({"clip_id":"01K30000000000000000000031","edge":"Head",)"
                 R"("delta":{"value":-5,"rate":25}})");
        const Document& afterHead = backend.CurrentDocument();
        Check(
            head.ok &&
                afterHead.FindClip("01K30000000000000000000031")->duration ==
                    RationalTime{15, 25} &&
                afterHead.FindClip("01K30000000000000000000033")->duration ==
                    RationalTime{15, 25} &&
                afterHead.FindClip("01K30000000000000000000032")->timeline_in ==
                    RationalTime{15, 25} &&
                afterHead.FindClip("01K30000000000000000000034")->timeline_in ==
                    RationalTime{15, 25},
            "the first head at position 0 lengthens and pushes the rest: " +
                head.message);
    }

    // QC-2026-09 C1 -- insert_clip only used to ripple the track it targets;
    // an A/V-linked partner living on another track was silently left
    // behind. sync_track_ids closes that gap the same way remove_clip's
    // does, with the same omission warning as ripple_trim above.
    {
        const Ulid sourceId = "01K30000000000000000000031";
        Document doc;
        doc.sources = {{sourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip v1{
            "01K30000000000000000000032", sourceId, {0, 25}, {10, 25}, {0, 25}};
        DocumentClip a1{
            "01K30000000000000000000033", sourceId, {0, 25}, {10, 25}, {0, 25}};
        const Ulid v1TrackId = "01K30000000000000000000034";
        const Ulid a1TrackId = "01K30000000000000000000035";
        doc.sequence.tracks = {
            {v1TrackId, "video", 0, {v1}},
            {a1TrackId, "audio", 1, {a1}},
        };
        const std::string insertArguments =
            R"({"track_id":")" + v1TrackId + R"(","source_id":")" + sourceId +
            R"(","source_in":{"value":100,"rate":25},)"
            R"("duration":{"value":5,"rate":25},)"
            R"("timeline_in":{"value":0,"rate":25})";

        McpToolRegistry registry;
        InMemoryBackend backend(doc);
        const std::string before = backend.CurrentDocument().SaveToString();
        std::string parseFailure;
        const McpToolCallOutcome omitted = [&] {
            mcp_json::Value arguments;
            Check(mcp_json::Value::Parse(insertArguments + "}", arguments,
                                         parseFailure),
                  "insert_clip arguments parse: " + parseFailure);
            return registry.Call(backend, "insert_clip", arguments);
        }();
        Check(omitted.ok, "insert_clip applies: " + omitted.message);
        // AGENT-2026-09 A34 -- an insert moves no other track (a cutaway,
        // a sound laid beside its picture) and warns instead.
        Check(backend.CurrentDocument().FindClip(a1.id)->timeline_in ==
                  RationalTime{0, 25},
              "unlisted A1 is not shifted without sync_track_ids");
        mcp_json::Value omittedResult;
        std::string resultError;
        Check(mcp_json::Value::Parse(omitted.result_json, omittedResult,
                                     resultError),
              "insert_clip result is JSON: " + resultError);
        Check(omittedResult.Find("warning") != nullptr &&
                  omittedResult.Find("warning_track_ids") != nullptr,
              "omitting sync_track_ids warns about downstream A1");
        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage) &&
                backend.CurrentDocument().SaveToString() == before,
            "insert undo restores V1/A1 byte-identically");

        InMemoryBackend aligned(doc);
        const McpToolCallOutcome synced = [&] {
            mcp_json::Value arguments;
            Check(mcp_json::Value::Parse(insertArguments +
                                             R"(,"sync_track_ids":[")" +
                                             a1TrackId + R"("]})",
                                         arguments, parseFailure),
                  "synced insert_clip arguments parse: " + parseFailure);
            return registry.Call(aligned, "insert_clip", arguments);
        }();
        Check(synced.ok, "synced insert_clip applies: " + synced.message);
        Check(aligned.CurrentDocument().FindClip(a1.id)->timeline_in ==
                  RationalTime{5, 25},
              "explicit sync_track_ids keeps A1 aligned with the ripple");
        mcp_json::Value syncedResult;
        Check(mcp_json::Value::Parse(synced.result_json, syncedResult,
                                     resultError) &&
                  syncedResult.Find("warning") == nullptr,
              "explicit synchronization suppresses the omission warning");
        Check(
            aligned.Log().Undo(const_cast<Document&>(aligned.CurrentDocument()),
                               undoError, undoMessage) &&
                aligned.CurrentDocument().SaveToString() == before,
            "synced insert undo restores V1/A1 byte-identically");
    }

    // IA5 -- ROADMAP.md. The session a running server keeps: replays and
    // revisions over the in-memory backend, the refusals a surface without a
    // server gives, and a silent connection that no longer holds the loop.
    {
        const McpToolRegistry registry;
        InMemoryBackend plain(fixture);
        mcp_json::Value arguments;
        std::string parseError;
        for (const std::string& key :
             {std::string(kMcpAsyncKey), std::string(kMcpIdempotencyKey),
              std::string(kMcpExpectedRevisionKey)}) {
            Check(mcp_json::Value::Parse(
                      "{\"" + key + "\":" +
                          (key == kMcpAsyncKey ? "true" : "\"x\"") + "}",
                      arguments, parseError),
                  "session key arguments parse");
            const McpToolCallOutcome outcome =
                registry.Call(plain, "transcribe_timeline", arguments);
            CheckFailureEnvelope(outcome, "'" + key + "' without a server",
                                 "UnsupportedOperation");
        }
        const McpToolCallOutcome noTasks =
            registry.Call(plain, "list_tasks", mcp_json::Value::MakeObject());
        CheckFailureEnvelope(noTasks, "list_tasks without a server",
                             "UnsupportedOperation");
        const auto schema =
            std::find_if(registry.Tools().begin(), registry.Tools().end(),
                         [](const McpTool& tool) {
                             return tool.name == "transcribe_media";
                         });
        Check(schema != registry.Tools().end() && schema->background &&
                  schema->input_schema_json.find("\"async\"") !=
                      std::string::npos &&
                  schema->input_schema_json.find("\"idempotency_key\"") !=
                      std::string::npos,
              "a long analysis advertises async and idempotency_key");
        // PRD40 -- ROADMAP.md. And so does an export, whose answer is lost
        // like an edit's.
        for (const std::string exportTool :
             {"export_timeline", "export_timelines"}) {
            const auto exported = std::find_if(
                registry.Tools().begin(), registry.Tools().end(),
                [&](const McpTool& tool) { return tool.name == exportTool; });
            Check(exported != registry.Tools().end() && exported->idempotent &&
                      exported->background &&
                      exported->input_schema_json.find("\"idempotency_key\"") !=
                          std::string::npos,
                  exportTool + " advertises idempotency_key");
        }

        InMemoryBackend projectBackend(fixture, true);
        McpServer sessionServer(projectBackend);
        int id = 900;
        const auto call = [&](const std::string& tool,
                              const std::string& args) {
            std::string response;
            sessionServer.HandleJsonRpc(
                "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++id) +
                    ",\"method\":\"tools/call\",\"params\":{\"name\":\"" +
                    tool + "\",\"arguments\":" + args + "}}",
                response);
            mcp_json::Value parsed;
            mcp_json::Value payload;
            std::string failure;
            if (mcp_json::Value::Parse(response, parsed, failure)) {
                const mcp_json::Value* content =
                    parsed.Find("result")->Find("content");
                mcp_json::Value::Parse(
                    content->AsArray()[0].Find("text")->AsString(), payload,
                    failure);
            }
            return payload;
        };
        const auto text = [](const mcp_json::Value& value,
                             const std::string& key) {
            const mcp_json::Value* field = value.Find(key);
            return field != nullptr && field->IsString() ? field->AsString()
                                                         : std::string();
        };
        const auto timelines = [&] {
            const mcp_json::Value listed = call("list_timelines", "{}");
            const mcp_json::Value* array = listed.Find("timelines");
            return array != nullptr ? array->AsArray().size() : size_t{0};
        };
        const size_t before = timelines();
        const std::string add =
            "{\"name\":\"Montage\",\"width\":1920,\"height\":1080,"
            "\"frame_rate\":{\"num\":25,\"den\":1},\"idempotency_key\":\"k\"}";
        const mcp_json::Value first = call("add_timeline", add);
        const mcp_json::Value second = call("add_timeline", add);
        const mcp_json::Value* replayed = second.Find("replayed");
        Check(first.Find("replayed") == nullptr && replayed != nullptr &&
                  replayed->AsBool() && timelines() == before + 1,
              "an add_timeline sent twice with one key creates one timeline: " +
                  second.Dump());
        const std::string revision =
            text(call("list_timelines", "{}"), "revision");
        Check(revision.size() == 16 && revision == text(first, "revision"),
              "the revision names the state after the last edit");
        const mcp_json::Value refused =
            call("add_timeline",
                 "{\"name\":\"B\",\"width\":1920,\"height\":1080,"
                 "\"frame_rate\":{\"num\":25,\"den\":1},"
                 "\"expected_revision\":\"0000000000000000\"}");
        Check(
            text(refused, "error") == "StaleRevision" &&
                timelines() == before + 1,
            "an edit expecting another revision is refused: " + refused.Dump());
        const mcp_json::Value async =
            call("transcribe_media",
                 "{\"media_id\":\"01K30000000000000000000001\","
                 "\"async\":true}");
        Check(text(async, "error") == "UnsupportedOperation" &&
                  text(async, "detail").find("background") != std::string::npos,
              "a backend that cannot fork refuses a task by name: " +
                  async.Dump());

        // A client that connects and says nothing, then one that asks.
        sessionServer.SetIdleTimeoutMilliseconds(200);
        std::string startError;
        Check(sessionServer.Start(0, startError),
              "session server starts: " + startError);
        const int silent = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(sessionServer.Port()));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(::connect(silent, reinterpret_cast<sockaddr*>(&address),
                        sizeof(address)) == 0,
              "a silent client connects");
        const std::string answer =
            HttpPostJson(sessionServer.Port(), "/mcp",
                         R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
        ::close(silent);
        Check(StatusLine(answer).find("200") != std::string::npos,
              "the server answers past a silent connection");
        sessionServer.Stop();
    }

    // MOTION-2026-09 M3 -- compositions: list, describe, add, edit as one
    // undo step, named refusals, and the renderer this process lacks.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& tool,
                              const std::string& arguments) {
            mcp_json::Value parsed;
            std::string error;
            Check(mcp_json::Value::Parse(arguments, parsed, error),
                  tool + " arguments parse: " + error);
            return registry.Call(backend, tool, parsed);
        };
        for (const char* tool :
             {"list_motion_compositions", "describe_motion_composition",
              "add_motion_composition", "remove_motion_composition",
              "edit_motion_composition", "render_motion_frame",
              "render_motion_video"}) {
            Check(std::any_of(
                      registry.Tools().begin(), registry.Tools().end(),
                      [&](const McpTool& entry) { return entry.name == tool; }),
                  std::string("the catalog offers ") + tool);
        }
        const McpToolCallOutcome empty = call("list_motion_compositions", "{}");
        Check(
            empty.ok && empty.result_json == R"({"compositions":[]})",
            "a project without compositions lists none: " + empty.result_json);

        const McpToolCallOutcome added =
            call("add_motion_composition",
                 R"({"name":"Carton","definition":{"width":1080,"height":1920,)"
                 R"("fps":25,"duration":50,"props":{"titre":{"type":"text",)"
                 R"("default":"Titre"},"taille":{"type":"number","default":80,)"
                 R"("min":10,"max":200}},"root":{"type":"fill","children":[)"
                 R"({"type":"text","id":"titre","content":{"prop":"titre"},)"
                 R"("style":{"fontSize":{"prop":"taille"},"lineHeight":1.4},)"
                 R"("animate":{"opacity":{"interpolate":{"frames":[0,6],)"
                 R"("values":[0,1]}}}}]}}})");
        Check(added.ok, "add_motion_composition applies: " + added.message);
        Check(backend.CurrentDocument().compositions.size() == 1,
              "the composition is in the document");
        if (added.ok && backend.CurrentDocument().compositions.size() == 1) {
            // A copy: the backend's document is replaced by every edit.
            const DocumentComposition stored =
                backend.CurrentDocument().compositions.front();
            mcp_json::Value result;
            std::string error;
            Check(mcp_json::Value::Parse(added.result_json, result, error) &&
                      result.Find("composition") != nullptr &&
                      result.Find("composition")->Dump() ==
                          MotionCompositionJson(stored),
                  "the result gives back the stored composition, uids "
                  "included: " +
                      added.result_json);
            const McpToolCallOutcome described = call(
                "describe_motion_composition",
                R"({"composition_id":")" + stored.id.substr(0, 20) + R"("})");
            Check(described.ok &&
                      described.result_json == MotionCompositionJson(stored),
                  "describe reads the stored composition by an id prefix");

            const std::string rootUid =
                stored.definition.Find("root")->Find("uid")->AsString();
            const std::string before = backend.CurrentDocument().SaveToString();
            const size_t steps = backend.Log().AppliedCount();
            const McpToolCallOutcome edited = call(
                "edit_motion_composition",
                R"({"composition_id":")" + stored.id +
                    R"(","edits":[{"op":"set_node","parent":")" + rootUid +
                    R"(","node":{"type":"text","id":"date","content":"Date"}},)"
                    R"({"op":"set_style","node":"date","property":"marginTop",)"
                    R"("value":12.5},{"op":"set_props","props":{"taille":120}}]})");
            Check(edited.ok, "three edits apply: " + edited.message);
            Check(backend.Log().AppliedCount() == steps + 1,
                  "as one undo step");
            const DocumentComposition now =
                backend.CurrentDocument().compositions.front();
            MotionNodeLocation date;
            Check(FindMotionNodeByName(now.definition, "date", date) &&
                      MotionTreeAt(now.definition, date.path)
                              ->Find("style")
                              ->Find("marginTop")
                              ->Dump() == "12.5",
                  "a later edit names the node an earlier one posed");

            const auto refused = [&](const std::string& edits,
                                     const std::string& expected) {
                const std::string current =
                    backend.CurrentDocument().SaveToString();
                const McpToolCallOutcome outcome =
                    call("edit_motion_composition",
                         R"({"composition_id":")" + stored.id +
                             R"(","edits":)" + edits + "}");
                CheckFailureEnvelope(outcome, "edit " + edits, expected);
                Check(backend.CurrentDocument().SaveToString() == current,
                      "a refused edit changes nothing: " + edits);
            };
            refused(R"([{"op":"set_props","props":{"taille":201}}])",
                    "PropOutOfRange");
            refused(R"([{"op":"set_props","props":{"taille":"grand"}}])",
                    "PropTypeMismatch");
            refused(R"([{"op":"set_props","props":{"couleur":"#fff"}}])",
                    "UnknownProp");
            refused(
                R"([{"op":"set_style","node":"absent","property":"opacity","value":1}])",
                "UnknownMotionNode");
            refused(
                R"([{"op":"set_keyframe","keyframe":"01K7000000000000000000ZZZZ","frame":3,"value":1}])",
                "UnknownKeyframe");
            refused(
                R"([{"op":"set_style","node":"date","property":"paddingX","value":4}])",
                "InvalidMotion");
            // The whole list is refused when one edit is.
            refused(R"([{"op":"set_props","props":{"taille":150}},)"
                    R"({"op":"set_props","props":{"taille":999}}])",
                    "PropOutOfRange");
            CheckFailureEnvelope(
                call("describe_motion_composition",
                     R"({"composition_id":"01K7000000000000000000ZZZZ"})"),
                "an unknown composition", "UnknownComposition");

            Check(call("undo", "{}").ok &&
                      backend.CurrentDocument().SaveToString() == before,
                  "undo takes the three edits back, byte for byte");

            // M6 -- versions and formats through the same tool, one undo
            // step, the prop refusals of M3.
            const size_t versionSteps = backend.Log().AppliedCount();
            const McpToolCallOutcome versions = call(
                "edit_motion_composition",
                R"({"composition_id":")" + stored.id +
                    R"(","edits":[{"op":"set_format","format":"16x9",)"
                    R"("width":1920,"height":1080,"props":{"taille":150}},)"
                    R"({"op":"set_variant","variant":"court",)"
                    R"("props":{"titre":"Court"}},{"op":"set_variant",)"
                    R"("variant":"long","props":{"titre":"Bien plus long"}}]})");
            Check(
                versions.ok && backend.Log().AppliedCount() == versionSteps + 1,
                "set_format and set_variant apply as one step: " +
                    versions.message);
            const McpToolCallOutcome listed =
                call("list_motion_compositions", "{}");
            Check(listed.ok &&
                      listed.result_json.find(
                          R"("variants":["court","long"],"formats":[)"
                          R"({"name":"16x9","width":1920,"height":1080}])") !=
                          std::string::npos,
                  "the list names the versions and formats: " +
                      listed.result_json);
            refused(R"([{"op":"set_variant","variant":"grand",)"
                    R"("props":{"taille":180}}])",
                    "InvalidMotion");
            refused(R"([{"op":"set_variant","variant":"x",)"
                    R"("props":{"date":"1er mai"}}])",
                    "UnknownProp");
            refused(R"([{"op":"set_variant","variant":"x",)"
                    R"("props":{"titre":5}}])",
                    "PropTypeMismatch");
            refused(R"([{"op":"set_format","format":"carre","width":1080,)"
                    R"("height":1080,"props":{"taille":500}}])",
                    "PropOutOfRange");
            refused(R"([{"op":"set_variant","variant":"moyen"}])",
                    "UnknownVariant");
            refused(R"([{"op":"set_format","format":"carre","width":1080}])",
                    "ValidationFailed");
            refused(R"([{"op":"set_format","format":"16:9","width":16,)"
                    R"("height":9}])",
                    "InvalidMotion");
            Check(call("edit_motion_composition",
                       R"({"composition_id":")" + stored.id +
                           R"(","edits":[{"op":"set_variant",)"
                           R"("variant":"long"}]})")
                      .ok,
                  "set_variant without props removes the version");

            // REMOTION-PARITY-2026-09 REM5 -- keys by frame, as the visual
            // editor sets them: a first key, a second, an easing, one step.
            const size_t keySteps = backend.Log().AppliedCount();
            const McpToolCallOutcome keyed = call(
                "edit_motion_composition",
                R"({"composition_id":")" + stored.id +
                    R"(","edits":[{"op":"key","node":"titre",)"
                    R"("property":"translateY","frame":0,"value":40},)"
                    R"({"op":"key","node":"titre","property":"translateY",)"
                    R"("frame":12,"value":0},{"op":"ease","node":"titre",)"
                    R"("property":"translateY","easing":"ease-out"}]})");
            const McpToolCallOutcome keyedDescribed =
                call("describe_motion_composition",
                     R"({"composition_id":")" + stored.id + R"("})");
            Check(keyed.ok && backend.Log().AppliedCount() == keySteps + 1 &&
                      keyedDescribed.result_json.find(
                          R"("translateY":{"interpolate":{"frames":[0,12],)"
                          R"("values":[40,0],)") != std::string::npos &&
                      keyedDescribed.result_json.find(
                          R"("easing":"ease-out")") != std::string::npos,
                  "key and ease build the animation in one step: " +
                      keyed.message + " " + keyedDescribed.result_json);
            // MOTION-BIBLIOTHEQUE-2026-10 MB5 -- one segment eased, by the
            // frame of the key it leaves: the exit takes its own curve, the
            // entrance keeps its own, and the list reads back.
            const McpToolCallOutcome exited = call(
                "edit_motion_composition",
                R"({"composition_id":")" + stored.id +
                    R"(","edits":[{"op":"key","node":"titre",)"
                    R"("property":"translateY","frame":30,"value":-40},)"
                    R"({"op":"ease","node":"titre","property":"translateY",)"
                    R"("easing":"ease-in","frame":12}]})");
            const std::string exitedDescribed =
                call("describe_motion_composition",
                     R"({"composition_id":")" + stored.id + R"("})")
                    .result_json;
            Check(exited.ok &&
                      exitedDescribed.find(R"("frames":[0,12,30],)") !=
                          std::string::npos &&
                      exitedDescribed.find(
                          R"("easing":["ease-out","ease-in"])") !=
                          std::string::npos,
                  "ease with a frame curves one segment: " + exited.message +
                      " " + exitedDescribed);
            refused(R"([{"op":"ease","node":"titre","property":"translateY",)"
                    R"("easing":"ease-in","frame":30}])",
                    "InvalidOperation");
            refused(R"([{"op":"ease","node":"titre","property":"translateY",)"
                    R"("easing":"ease-in","frame":"12"}])",
                    "ValidationFailed");
            Check(call("edit_motion_composition",
                       R"({"composition_id":")" + stored.id +
                           R"(","edits":[{"op":"unkey","node":"titre",)"
                           R"("property":"translateY","frame":30}]})")
                      .ok,
                  "the exit key removed, its segment with it");
            Check(call("edit_motion_composition",
                       R"({"composition_id":")" + stored.id +
                           R"(","edits":[{"op":"unkey","node":"titre",)"
                           R"("property":"translateY","frame":12}]})")
                          .ok &&
                      call("describe_motion_composition",
                           R"({"composition_id":")" + stored.id + R"("})")
                              .result_json.find(
                                  R"("frames":[0],"values":[40])") !=
                          std::string::npos,
                  "unkey removes one key, the other holding its value");
            refused(R"([{"op":"unkey","node":"titre","property":"translateY",)"
                    R"("frame":5}])",
                    "InvalidOperation");
            refused(
                R"([{"op":"ease","node":"titre","property":"letterSpacing",)"
                R"("easing":"linear"}])",
                "InvalidOperation");

            CheckFailureEnvelope(
                call("render_motion_frame",
                     R"({"composition_id":")" + stored.id + R"(","frame":0})"),
                "render without a renderer in this process",
                "UnsupportedOperation");
            CheckFailureEnvelope(
                call("render_motion_video",
                     R"({"composition_id":")" + stored.id +
                         R"(","output":"/tmp/x_{variant}.mov"})"),
                "a video without a renderer in this process",
                "UnsupportedOperation");

            Check(call("remove_motion_composition",
                       R"({"composition_id":")" + stored.id + R"("})")
                          .ok &&
                      backend.CurrentDocument().compositions.empty(),
                  "remove_motion_composition removes it");
        }
    }

    // MOTIF2 -- docs/roadmap/MOTIF-2026-10.md.
    CheckMotifs(fixture);
    // PA5, PA9 -- docs/roadmap/PARITE-2026-11.md.
    CheckTranslationAndChanges(fixture);
    // PRD14 -- ROADMAP.md.
    CheckArgumentRules();
    CheckFirstClip();
    CheckCatalogTolerance(fixture);
    // PRD15 -- ROADMAP.md.
    CheckToolClassesAndAnnotations(fixture);
    CheckRequiredArguments(fixture);
    CheckDescriptionLengths();
    CheckCatalogWeightPosts();
    // PF1 -- docs/roadmap/PROFIL-2026-11.md.
    CheckToolProfiles(fixture);
    CheckDeclaredSetsAndSmallFixes(fixture);
    CheckSetClipEnabled(fixture);
    CheckSplitLinkGroups(fixture);
    // PRD16 -- ROADMAP.md.
    CheckExportPresets(fixture);
    CheckExportTasksAndCallerPaths();
    // PRD17 -- ROADMAP.md.
    CheckSeriesArguments();
    // PRD35 -- ROADMAP.md.
    CheckEngineLogCalls(fixture);
    CheckTimelineHistory(fixture);
    // FC7 -- docs/roadmap/FILMCRAFT-2026-10.md.
    CheckEdlTools(fixture);
    CheckSeriesFindings(fixture);
    // DC1 -- docs/roadmap/DECOUPAGE-2026-10.md.
    CheckInspectCutDispatch(fixture);
    CheckReviewCutsDispatch(fixture);
    CheckMusicStructureDispatch();
    CheckConformColorManagementDispatch();
    CheckSplitMulticamGroupDispatch(fixture);
    CheckMeasureAudioOffsetDispatch();
    CheckAlignWordsDispatch();
    CheckCaptionLegibilityDispatch(fixture);
    CheckPlanReadsRushCaches();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All MCP tool tests passed\n";
    return 0;
}
