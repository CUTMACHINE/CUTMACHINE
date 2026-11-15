#include "Cli.h"

#include "AggregateMembers.h"
#include "BeatDetection.h"
#include "BoundaryAir.h"
#include "BuildInfo.h"
#include "CanonicalHash.h"
#include "CaptionBreakModel.h"
#include "ClipAnimation.h"
#include "ClipSpeed.h"
#include "ColorMeasure.h"
#include "CutInspectionRender.h"
#include "CutReview.h"
#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "EditPlan.h"
#include "Export.h"
#include "FfmpegBinary.h"
#include "FfmpegCapabilities.h"
#include "IdResolver.h"
#include "Ingest.h"
#include "InterviewShort.h"
#include "IsolatedFrames.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "MfaAlignment.h"
#include "MotionClip.h"
#include "MotionLibraryBase.h"
#include "Operations.h"
#include "PauseTightening.h"
#include "Procedures.h"
#include "ProcessLauncher.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "SeparateSound.h"
#include "SequenceFormat.h"
#include "SetupCheck.h"
#include "ShotQuality.h"
#include "SourceAddress.h"
#include "SpeakerDiarization.h"
#include "SpeechOnset.h"
#include "Subtitles.h"
#include "Timeline.h"
#include "TimelineCheck.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"
#include "TimelineTranscription.h"
#include "TranscriptAlignment.h"
#include "Transcription.h"
#include "Ulid.h"
#include "WordAlignment.h"
#include "WorkCounters.h"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus) {
    return FailCliCommandWithResult(errorCode, detail, {}, output, exitStatus);
}

int FailCliCommandWithResult(const std::string& errorCode,
                             const std::string& detail,
                             const std::string& resultJson, std::string& output,
                             int exitStatus) {
    mcp_json::Value result;
    std::string parseError;
    const bool hasResult =
        !resultJson.empty() &&
        mcp_json::Value::Parse(resultJson, result, parseError) &&
        result.IsObject();
    // PRD1 -- a command that needed FFmpeg and had none says so by name,
    // whichever code its caller gave the sentence it relayed.
    // PRD20 -- and so does one whose project path leads nowhere: IoError,
    // as for any file that is not there, where each command used to give
    // the name of its own load failure (ParseError, InvalidDocument).
    std::string named = ffmpeg::RefusalName(detail);
    // PRD21 -- and so does one that needed MFA and found it nowhere.
    if (named.empty()) {
        named = MfaRefusalName(detail);
    }
    // PA3 -- and so does one that needed the diarization model.
    if (named.empty()) {
        named = DiarizationRefusalName(detail);
    }
    if (named.empty() && IsMissingProjectMessage(detail)) {
        named = "IoError";
    }
    output = "{\"ok\":false,\"error\":\"" +
             mcp_json::EscapeJsonString(named.empty() ? errorCode : named) +
             "\",\"detail\":\"" + mcp_json::EscapeJsonString(detail) + "\"" +
             (hasResult ? ",\"result\":" + result.Dump() : "") + "}\n";
    return exitStatus;
}

bool ReadCliCommandResult(const std::string& output, bool commandSucceeded,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) {
    mcp_json::Value value;
    std::string parseError;
    if (!mcp_json::Value::Parse(output, value, parseError)) {
        errorName = "IoError";
        message = "malformed command output: " + parseError;
        return false;
    }
    if (!commandSucceeded) {
        const mcp_json::Value* errorField = value.Find("error");
        const mcp_json::Value* detailField = value.Find("detail");
        errorName = (errorField != nullptr && errorField->IsString())
                        ? errorField->AsString()
                        : "InvalidOperation";
        message = (detailField != nullptr && detailField->IsString())
                      ? detailField->AsString()
                      : "operation failed";
        // PRD20 -- what a refusal carries beside its sentence
        // (FailCliCommandWithResult) reaches the caller too: a tool's
        // outcome attaches it as `result`, so the files an ingest could not
        // read are named over MCP as they are on the command line.
        const mcp_json::Value* resultField = value.Find("result");
        if (resultField != nullptr && resultField->IsObject()) {
            resultJson = resultField->Dump();
        }
        return false;
    }
    resultJson = output;
    return true;
}

std::string WithCliUsage(const std::string& output, int exitStatus,
                         const std::string& usage) {
    mcp_json::Value value;
    std::string parseError;
    if (exitStatus != 2 || usage.empty() ||
        !mcp_json::Value::Parse(output, value, parseError) ||
        !value.IsObject()) {
        return output;
    }
    const mcp_json::Value* error = value.Find("error");
    const mcp_json::Value* detail = value.Find("detail");
    if (error == nullptr || !error->IsString() ||
        error->AsString() != "ValidationFailed" || detail == nullptr ||
        !detail->IsString() ||
        detail->AsString().find("usage: ") != std::string::npos) {
        return output;
    }
    const mcp_json::Value* result = value.Find("result");
    std::string decorated;
    FailCliCommandWithResult(
        error->AsString(), detail->AsString() + "; " + usage,
        result != nullptr && result->IsObject() ? result->Dump()
                                                : std::string(),
        decorated, exitStatus);
    return decorated;
}

std::string WithCliOk(const std::string& output) {
    constexpr char kSpace[] = " \t\r\n";
    const size_t open = output.find_first_not_of(kSpace);
    if (open == std::string::npos || output[open] != '{') {
        return output;
    }
    const size_t first = output.find_first_not_of(kSpace, open + 1);
    if (first == std::string::npos || output.compare(first, 4, "\"ok\"") == 0) {
        return output;
    }
    return output.substr(0, open + 1) + "\"ok\":true" +
           (output[first] == '}' ? "" : ",") + output.substr(open + 1);
}

namespace {

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string ErrorJson(EditError error, const std::string& detail) {
    std::string output;
    FailCliCommand(EditErrorName(error), detail, output);
    return output;
}

bool ReadFile(const std::string& path, std::string& contents,
              std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        message = "unable to open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        message = "unable to read '" + path + "'";
        return false;
    }
    contents = buffer.str();
    return true;
}

bool WriteFile(const std::filesystem::path& path, const std::string& contents,
               std::string& message) {
    const int descriptor =
        open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0) {
        message = "unable to create '" + path.string() + "'";
        return false;
    }
    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + written,
                                    contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int savedError = errno;
            close(descriptor);
            message = "unable to write '" + path.string() +
                      "': " + std::strerror(savedError);
            return false;
        }
        written += static_cast<size_t>(count);
    }
    // fsync is the portable baseline. macOS may still defer drive-cache
    // flushing, so request F_FULLFSYNC there when the filesystem supports it.
#ifdef F_FULLFSYNC
    if (fcntl(descriptor, F_FULLFSYNC) != 0 && fsync(descriptor) != 0) {
#else
    if (fsync(descriptor) != 0) {
#endif
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message =
            "unable to close '" + path.string() + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

bool SyncDirectory(const std::filesystem::path& path, std::string& message) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        message = "unable to open directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    if (fsync(descriptor) != 0) {
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize directory '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message = "unable to close directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    return true;
}

void RemoveIfPresent(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool Rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::string& message) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    message = "unable to rename '" + from.string() + "' to '" + to.string() +
              "': " + error.message();
    return false;
}

struct CommitArtifact {
    std::filesystem::path path;
    std::string contents;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool existed = false;
    bool backed_up = false;
    bool committed = false;
    bool remove = false;
    bool unchanged = false;
};

std::string HexEncode(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 0xf]);
    }
    return output;
}

bool HexDecode(const std::string& input, std::string& output) {
    if (input.size() % 2 != 0) return false;
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    output.clear();
    output.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2) {
        const int high = digit(input[index]);
        const int low = digit(input[index + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::filesystem::path CommonDirectory(
    const std::vector<CommitArtifact>& artifacts) {
    std::filesystem::path common =
        std::filesystem::absolute(artifacts.front().path).parent_path();
    for (size_t index = 1; index < artifacts.size(); ++index) {
        const std::filesystem::path parent =
            std::filesystem::absolute(artifacts[index].path).parent_path();
        while (!common.empty()) {
            const std::filesystem::path relative =
                parent.lexically_relative(common);
            if (!relative.empty() && *relative.begin() != "..") break;
            if (parent == common) break;
            common = common.parent_path();
        }
    }
    return common;
}

bool IsWithinDirectory(const std::filesystem::path& directory,
                       const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalDirectory =
        std::filesystem::weakly_canonical(directory, error);
    if (error) return false;
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(path.parent_path(), error);
    if (error) return false;
    const std::filesystem::path relative =
        canonicalParent.lexically_relative(canonicalDirectory);
    return canonicalParent == canonicalDirectory ||
           (!relative.empty() && *relative.begin() != "..");
}

bool IsTransactionLeftover(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    const size_t marker = filename.rfind(".cutmachine-");
    if (marker == std::string::npos) return false;
    const std::string suffix = filename.substr(marker + 12);
    if (suffix.size() != 30 || suffix[26] != '.') return false;
    for (size_t index = 0; index < 26; ++index)
        if (!std::isalnum(static_cast<unsigned char>(suffix[index])))
            return false;
    return suffix.substr(27) == "tmp" || suffix.substr(27) == "bak";
}

bool RecoverTransaction(const std::filesystem::path& directory,
                        std::string& message) {
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    std::error_code existsError;
    if (!std::filesystem::exists(marker, existsError)) {
        if (existsError) {
            message = "unable to inspect transaction marker: " +
                      existsError.message();
            return false;
        }
        return true;
    }
    std::string journal;
    if (!ReadFile(marker.string(), journal, message)) return false;
    std::istringstream input(journal);
    std::string version;
    std::string nonce;
    if (!std::getline(input, version) ||
        version != "CUTMACHINE_TRANSACTION_V1" || !std::getline(input, nonce) ||
        nonce.size() != 38 || nonce.rfind(".cutmachine-", 0) != 0) {
        message = "invalid interrupted transaction marker";
        return false;
    }
    for (size_t index = 12; index < nonce.size(); ++index)
        if (!std::isalnum(static_cast<unsigned char>(nonce[index]))) {
            message = "invalid interrupted transaction marker";
            return false;
        }
    struct RecoveryEntry {
        std::filesystem::path path;
        bool existed = false;
    };
    std::vector<RecoveryEntry> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != ' ') {
            message = "invalid interrupted transaction entry";
            return false;
        }
        std::string decoded;
        if ((line[0] != '0' && line[0] != '1') ||
            !HexDecode(line.substr(2), decoded)) {
            message = "invalid interrupted transaction entry";
            return false;
        }
        const std::filesystem::path relative(decoded);
        if (relative.empty() || relative.is_absolute()) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        for (const auto& component : relative)
            if (component == "..") {
                message = "unsafe interrupted transaction entry";
                return false;
            }
        const std::filesystem::path path = directory / relative;
        if (!IsWithinDirectory(directory, path)) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        entries.push_back({path, line[0] == '1'});
    }
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        const std::filesystem::path backup =
            item->path.string() + nonce + ".bak";
        const std::filesystem::path temporary =
            item->path.string() + nonce + ".tmp";
        std::error_code backupError;
        if (std::filesystem::exists(backup, backupError)) {
            RemoveIfPresent(item->path);
            if (!Rename(backup, item->path, message)) return false;
        } else if (backupError) {
            message = "unable to inspect transaction backup: " +
                      backupError.message();
            return false;
        } else if (!item->existed) {
            RemoveIfPresent(item->path);
        }
        RemoveIfPresent(temporary);
        if (!SyncDirectory(item->path.parent_path(), message)) return false;
    }
    if (!std::filesystem::remove(marker, existsError) || existsError) {
        message = "unable to remove recovered transaction marker: " +
                  existsError.message();
        return false;
    }
    return SyncDirectory(directory, message);
}

void RemoveTransactionLeftovers(const std::filesystem::path& directory) {
    std::error_code error;
    for (const std::filesystem::path& scan :
         {directory, directory / "Timelines"}) {
        if (!std::filesystem::is_directory(scan, error)) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator item(scan, error), end;
             !error && item != end; item.increment(error)) {
            if (item->is_regular_file(error) &&
                IsTransactionLeftover(item->path()))
                RemoveIfPresent(item->path());
            error.clear();
        }
    }
}

// PERF-2026-09. A generation is durable once its bytes are on disk -- and an
// artifact whose bytes are already exactly there is, by definition, already
// durable. Every commit hands us the whole package (project file, manifest,
// each timeline, each journal) even when one timeline moved, and each byte
// rewritten costs an F_FULLFSYNC plus two directory syncs: on an external
// USB/exFAT volume that is tens of milliseconds apiece, paid on the thread
// the editor draws from. Comparing first turns those into one cheap read.
// This weakens no guarantee in SAVING_ROADMAP.md S2: what is not rewritten
// is not changed, so it needs neither the journal nor a rollback entry.
bool ArtifactMatchesDisk(const CommitArtifact& artifact, bool exists) {
    if (artifact.remove) return !exists;
    if (!exists) return false;
    std::string existing;
    std::string ignored;
    return ReadFile(artifact.path.string(), existing, ignored) &&
           existing == artifact.contents;
}

// S2 -- SAVING_ROADMAP.md. Every byte and the rollback marker reach durable
// storage before an existing canonical destination is moved.
bool CommitArtifacts(std::vector<CommitArtifact> artifacts,
                     std::string& message) {
    if (artifacts.empty()) return true;
    const std::filesystem::path directory = CommonDirectory(artifacts);
    if (directory.empty() || directory == directory.root_path()) {
        message = "transaction artifacts do not share a safe directory";
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (!IsWithinDirectory(directory,
                               std::filesystem::absolute(artifact.path))) {
            message = "transaction artifact escapes its package directory";
            return false;
        }
    if (!RecoverTransaction(directory, message)) return false;
    RemoveTransactionLeftovers(directory);
    // Recovery first: until it has run, what is on disk may still be a
    // half-committed generation, and comparing against that would skip an
    // artifact that does need rewriting.
    for (CommitArtifact& artifact : artifacts) {
        std::error_code existsError;
        artifact.existed = std::filesystem::exists(artifact.path, existsError);
        if (existsError) {
            message = "unable to inspect '" + artifact.path.string() +
                      "': " + existsError.message();
            return false;
        }
        artifact.unchanged = ArtifactMatchesDisk(artifact, artifact.existed);
    }
    artifacts.erase(std::remove_if(artifacts.begin(), artifacts.end(),
                                   [](const CommitArtifact& artifact) {
                                       return artifact.unchanged;
                                   }),
                    artifacts.end());
    if (artifacts.empty()) return true;
    work_counters::Count(work_counters::Work::kArtifactCommit,
                         artifacts.size());
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    const std::filesystem::path markerTemporary =
        marker.string() + nonce + ".tmp";
    const auto cleanupTemporary = [&] {
        for (const CommitArtifact& artifact : artifacts)
            RemoveIfPresent(artifact.temporary);
    };
    const auto rollback = [&] {
        std::string ignored;
        for (auto item = artifacts.rbegin(); item != artifacts.rend(); ++item) {
            if (item->committed) RemoveIfPresent(item->path);
            if (item->backed_up) Rename(item->backup, item->path, ignored);
        }
        cleanupTemporary();
    };
    for (CommitArtifact& artifact : artifacts) {
        artifact.temporary = artifact.path.string() + nonce + ".tmp";
        artifact.backup = artifact.path.string() + nonce + ".bak";
        if (!artifact.remove &&
            !WriteFile(artifact.temporary, artifact.contents, message)) {
            cleanupTemporary();
            return false;
        }
    }
    std::ostringstream journal;
    journal << "CUTMACHINE_TRANSACTION_V1\n" << nonce << '\n';
    for (const CommitArtifact& artifact : artifacts) {
        const std::filesystem::path relative =
            std::filesystem::absolute(artifact.path)
                .lexically_relative(directory);
        journal << (artifact.existed ? '1' : '0') << ' '
                << HexEncode(relative.generic_string()) << '\n';
    }
    if (!WriteFile(markerTemporary, journal.str(), message) ||
        !Rename(markerTemporary, marker, message) ||
        !SyncDirectory(directory, message)) {
        cleanupTemporary();
        RemoveIfPresent(markerTemporary);
        RemoveIfPresent(marker);
        return false;
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.existed) {
            if (!Rename(artifact.path, artifact.backup, message)) {
                rollback();
                return false;
            }
            artifact.backed_up = true;
            if (!SyncDirectory(artifact.path.parent_path(), message)) {
                rollback();
                return false;
            }
        }
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.remove) continue;
        if (!Rename(artifact.temporary, artifact.path, message)) {
            rollback();
            return false;
        }
        artifact.committed = true;
        if (!SyncDirectory(artifact.path.parent_path(), message)) {
            rollback();
            return false;
        }
    }
    std::error_code markerError;
    if (!std::filesystem::remove(marker, markerError) || markerError) {
        message =
            "unable to remove transaction marker: " + markerError.message();
        rollback();
        return false;
    }
    if (!SyncDirectory(directory, message)) {
        std::string ignored;
        if (WriteFile(markerTemporary, journal.str(), ignored) &&
            Rename(markerTemporary, marker, ignored))
            SyncDirectory(directory, ignored);
        rollback();
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (artifact.backed_up) RemoveIfPresent(artifact.backup);
    RemoveTransactionLeftovers(directory);
    return true;
}

std::string DecimalSeconds(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(9)
           << (static_cast<long double>(time.value) /
               static_cast<long double>(time.rate));
    std::string text = output.str();
    while (text.size() > 2 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.push_back('0');
    return text;
}

void WriteTime(std::ostringstream& output, const RationalTime& time,
               const MediaRate& frameRate) {
    output << "{\"frames\":" << time.to_frames(frameRate.num, frameRate.den)
           << ",\"seconds\":" << DecimalSeconds(time) << '}';
}

// PRD14 -- ROADMAP.md. `"alias":"T1",` for a track listing, or nothing when
// the track has no alias (IdResolver.h says when). The spelling is
// IdResolver's own, so an alias shown here is one the tools resolve.
std::string TrackAliasMember(const std::vector<const DocumentTrack*>& tracks,
                             size_t trackOrdinal) {
    const std::string alias = TrackAlias(tracks, trackOrdinal);
    return alias.empty() ? std::string() : "\"alias\":\"" + alias + "\",";
}

MediaRate PresentationRate(const Document& document) {
    return document.sequence.frame_rate;
}

// Describe is what an agent reads back after it edits, so every member it
// lists is listed by hand below. QC-2026-09 A7 is what that cost once: gain
// and fades could be set through set_clip_audio and read back by nothing.
// A member added to one of these structs is a compile error here until
// describe shows it, or this comment says why it does not:
//   Document::version        the file format, not the edit
//   DocumentSource::path     shown as `file`; the library has the full path
//   ClipEffect::id           set_clip_effects assigns new IDs on every call
//   LibraryMedia::proxy_path derived and local to the machine
//   Document::compositions   project-wide, not the timeline: read by
//                            --describe-compositions and the MCP tools
//                            list_motion_compositions and
//                            describe_motion_composition (MOTION-2026-09 M3)
static_assert(AggregateMemberCount<Document>::value == 7,
              "Document gained a member: describe it or say why not");
static_assert(AggregateMemberCount<DocumentSequence>::value == 11,
              "DocumentSequence gained a member: describe it or say why not");
static_assert(AggregateMemberCount<DocumentTrack>::value == 9,
              "DocumentTrack gained a member: describe it or say why not");
static_assert(AggregateMemberCount<DocumentClip>::value == 26,
              "DocumentClip gained a member: describe it or say why not");
// R1 -- docs/roadmap/REEL-2026-09.md. What set_clip_framing sets, read back
// by WriteClipSettings below; a member added here is a compile error until
// it is written there.
static_assert(AggregateMemberCount<ClipFraming>::value == 8,
              "ClipFraming gained a member: describe it or say why not");
// R2 -- docs/roadmap/REEL-2026-09.md. What set_clip_animation sets, read
// back by WriteClipSettings below, spelled exactly as the tool takes it.
static_assert(AggregateMemberCount<ClipKeyframe>::value == 8,
              "ClipKeyframe gained a member: describe it or say why not");
static_assert(AggregateMemberCount<ClipParameterAnimation>::value == 2,
              "ClipParameterAnimation gained a member: describe it or say "
              "why not");
static_assert(AggregateMemberCount<ClipEffect>::value == 3,
              "ClipEffect gained a member: describe it or say why not");
// R5 -- docs/roadmap/REEL-2026-09.md. The colour a dip goes through is
// described beside the type, so --describe says which black a film opens on.
static_assert(AggregateMemberCount<DocumentTransition>::value == 8,
              "DocumentTransition gained a member: describe it or say why not");
static_assert(AggregateMemberCount<CaptionStyle>::value == 15,
              "CaptionStyle gained a member: describe it or say why not");
static_assert(AggregateMemberCount<MulticamGroup>::value == 4,
              "MulticamGroup gained a member: describe it or say why not");
static_assert(AggregateMemberCount<MulticamAngle>::value == 3,
              "MulticamAngle gained a member: describe it or say why not");
// D6 -- duration is described when the marker is a span, beside its time.
static_assert(AggregateMemberCount<DocumentMarker>::value == 6,
              "DocumentMarker gained a member: describe it or say why not");
// G7 -- white_balance_kelvin is described beside its source, the neutral of
// color.temperature for every clip that reads it, and in the library. G3 --
// color_input in the library, beside the tags it was read from. PL4 --
// shots as their count, described_shots: search_shots reads them whole.
// GN3 -- generation in the library, on a media a service made.
static_assert(AggregateMemberCount<LibraryMedia>::value == 28,
              "LibraryMedia gained a member: describe it or say why not");
static_assert(AggregateMemberCount<DocumentBin>::value == 3,
              "DocumentBin gained a member: describe it or say why not");
static_assert(AggregateMemberCount<DocumentSource>::value == 4,
              "DocumentSource gained a member: describe it or say why not");
static_assert(AggregateMemberCount<ColorManagementSettings>::value == 8,
              "ColorManagementSettings gained a member: describe it or say "
              "why not");

// Settings are spelled exactly as the tool that sets them takes them --
// exact fractions and {value, rate} times rather than the rounded frames
// and seconds describe gives positions -- so that what an agent reads can
// be sent back unchanged.
void WriteFraction(std::ostringstream& output, const EffectParamValue& value) {
    output << "{\"num\":" << value.num << ",\"den\":" << value.den << '}';
}

void WriteExactTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << '}';
}

const char* AlignmentName(TransitionAlignment alignment) {
    switch (alignment) {
        case TransitionAlignment::StartAtCut:
            return "StartAtCut";
        case TransitionAlignment::EndAtCut:
            return "EndAtCut";
        case TransitionAlignment::Center:
            break;
    }
    return "Center";
}

// What set_clip_effects, set_clip_opacity, set_clip_blur, set_clip_speed,
// set_clip_framing and set_clip_audio change. A neutral setting is omitted, as
// an empty link is: no key means no effect, full opacity, no blur, speed 1/1,
// the default framing, unity gain and no fade.
void WriteClipSettings(std::ostringstream& output, const DocumentClip& clip) {
    if (!clip.effects.empty()) {
        output << ",\"effects\":[";
        for (size_t index = 0; index < clip.effects.size(); ++index) {
            const ClipEffect& effect = clip.effects[index];
            if (index) output << ',';
            output << "{\"type\":\"" << EscapeJson(effect.type)
                   << "\",\"params\":{";
            size_t paramIndex = 0;
            for (const auto& [name, value] : effect.params) {
                if (paramIndex++) output << ',';
                output << '"' << EscapeJson(name) << "\":";
                WriteFraction(output, value);
            }
            output << "}}";
        }
        output << ']';
    }
    if (clip.opacity.num != clip.opacity.den) {
        output << ",\"opacity\":";
        WriteFraction(output, clip.opacity);
    }
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The blur's sigma in sequence
    // pixels, written only on a blurred clip, as set_clip_blur takes it.
    if (ClipHasBlur(clip)) {
        output << ",\"blur\":";
        WriteFraction(output, clip.blur);
    }
    // PA8 -- docs/roadmap/PARITE-2026-11.md. The mask, written only on a
    // masked clip, as set_clip_mask takes it.
    if (ClipHasMask(clip)) {
        output << ",\"mask\":{\"shape\":\"" << EscapeJson(clip.mask.shape)
               << "\",\"center_x\":";
        WriteFraction(output, clip.mask.center_x);
        output << ",\"center_y\":";
        WriteFraction(output, clip.mask.center_y);
        output << ",\"width\":";
        WriteFraction(output, clip.mask.width);
        output << ",\"height\":";
        WriteFraction(output, clip.mask.height);
        output << ",\"feather\":";
        WriteFraction(output, clip.mask.feather);
        output << ",\"inverted\":" << (clip.mask.inverted ? "true" : "false")
               << '}';
    }
    // R3 -- docs/roadmap/REEL-2026-09.md. Written only when the clip is not
    // at 1/1, like every other neutral setting here.
    if (clip.speed.num != clip.speed.den) {
        output << ",\"speed\":";
        WriteFraction(output, clip.speed);
    }
    // R3, criterion 4. Written only on a ramped clip, spelled exactly as
    // set_clip_speed takes it back.
    if (ClipHasSpeedRamp(clip)) {
        output << ",\"speed_ramp\":[";
        for (size_t key = 0; key < clip.speed_ramp.size(); ++key) {
            const ClipKeyframe& keyframe = clip.speed_ramp[key];
            if (key) output << ',';
            output << "{\"id\":\"" << EscapeJson(keyframe.id) << "\",\"time\":";
            WriteExactTime(output, keyframe.time);
            output << ",\"value\":";
            WriteFraction(output, keyframe.value);
            output << ",\"interpolation\":\""
                   << KeyframeInterpolationName(keyframe.interpolation) << '"'
                   << '}';
        }
        output << ']';
    }
    // R1 -- docs/roadmap/REEL-2026-09.md. Written only on a framed clip,
    // spelled exactly as set_clip_framing takes it back.
    if (ClipHasFraming(clip)) {
        output << ",\"framing\":{\"mode\":\""
               << (clip.framing.mode == FramingMode::Fill ? "fill" : "fit")
               << "\",\"scale\":";
        WriteFraction(output, clip.framing.scale);
        output << ",\"position_x\":";
        WriteFraction(output, clip.framing.position_x);
        output << ",\"position_y\":";
        WriteFraction(output, clip.framing.position_y);
        output << ",\"crop_left\":";
        WriteFraction(output, clip.framing.crop_left);
        output << ",\"crop_right\":";
        WriteFraction(output, clip.framing.crop_right);
        output << ",\"crop_top\":";
        WriteFraction(output, clip.framing.crop_top);
        output << ",\"crop_bottom\":";
        WriteFraction(output, clip.framing.crop_bottom);
        output << '}';
    }
    // R2 -- docs/roadmap/REEL-2026-09.md. Written only on a clip that
    // carries a key, spelled exactly as set_clip_animation takes it back.
    if (ClipHasAnimation(clip)) {
        output << ",\"animation\":[";
        for (size_t index = 0; index < clip.animation.size(); ++index) {
            const ClipParameterAnimation& entry = clip.animation[index];
            if (index) output << ',';
            output << "{\"parameter\":\""
                   << ClipAnimatedParameterName(entry.parameter)
                   << "\",\"keyframes\":[";
            for (size_t key = 0; key < entry.keyframes.size(); ++key) {
                const ClipKeyframe& keyframe = entry.keyframes[key];
                if (key) output << ',';
                output << "{\"id\":\"" << EscapeJson(keyframe.id)
                       << "\",\"time\":";
                WriteExactTime(output, keyframe.time);
                output << ",\"value\":";
                WriteFraction(output, keyframe.value);
                output << ",\"interpolation\":\""
                       << KeyframeInterpolationName(keyframe.interpolation)
                       << '"';
                if (keyframe.interpolation ==
                    KeyframeInterpolation::CubicBezier) {
                    output << ",\"easing\":[";
                    WriteFraction(output, keyframe.easing_x1);
                    output << ',';
                    WriteFraction(output, keyframe.easing_y1);
                    output << ',';
                    WriteFraction(output, keyframe.easing_x2);
                    output << ',';
                    WriteFraction(output, keyframe.easing_y2);
                    output << ']';
                }
                output << '}';
            }
            output << "]}";
        }
        output << ']';
    }
    // The same test Document::Validate applies: a {0, 2} gain is written.
    if (clip.audio_gain_db.num != 0 || clip.audio_gain_db.den != 1 ||
        clip.audio_fade_in.value != 0 || clip.audio_fade_out.value != 0) {
        output << ",\"audio\":{\"gain_db\":";
        WriteFraction(output, clip.audio_gain_db);
        output << ",\"fade_in\":";
        WriteExactTime(output, clip.audio_fade_in);
        output << ",\"fade_out\":";
        WriteExactTime(output, clip.audio_fade_out);
        output << '}';
    }
}

std::string Describe(const Document& document) {
    const MediaRate timelineRate = PresentationRate(document);
    Timeline timeline(document);
    const RationalTime duration = timeline.Duration();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"sequence\":{\"id\":\"" << EscapeJson(document.sequence.id)
           << "\",\"name\":\"" << EscapeJson(document.sequence.name)
           << "\",\"width\":" << document.sequence.width
           << ",\"height\":" << document.sequence.height << ",\"frame_rate\":\""
           << document.sequence.frame_rate.num << '/'
           << document.sequence.frame_rate.den
           << "\"},\"timeline\":{\"sources\":[";
    for (size_t index = 0; index < document.sources.size(); ++index) {
        if (index) output << ',';
        const DocumentSource& source = document.sources[index];
        output << "{\"id\":\"" << EscapeJson(source.id) << "\",\"file\":\""
               << EscapeJson(
                      std::filesystem::path(source.path).filename().string())
               << "\",\"frame_rate\":\"" << source.rate.num << '/'
               << source.rate.den << "\",\"duration\":";
        WriteTime(output, source.duration, source.rate);
        if (const LibraryMedia* media = document.FindLibraryMedia(source.id);
            media != nullptr && media->white_balance_kelvin > 0) {
            output << ",\"white_balance_kelvin\":"
                   << media->white_balance_kelvin;
        }
        output << '}';
    }
    output << "],\"tracks\":[";

    const std::vector<const DocumentTrack*> tracks =
        TracksInDescribeOrder(document.sequence);
    for (size_t trackOrdinal = 0; trackOrdinal < tracks.size();
         ++trackOrdinal) {
        if (trackOrdinal) output << ',';
        const DocumentTrack& track = *tracks[trackOrdinal];
        output << '{' << TrackAliasMember(tracks, trackOrdinal) << "\"id\":\""
               << EscapeJson(track.id) << "\",\"kind\":\""
               << EscapeJson(track.kind) << "\",\"index\":" << track.index
               << ",\"locked\":" << (track.locked ? "true" : "false")
               << ",\"sync_lock\":" << (track.sync_lock ? "true" : "false")
               << ",\"visible\":" << (track.visible ? "true" : "false")
               << ",\"muted\":" << (track.muted ? "true" : "false")
               << ",\"solo\":" << (track.solo ? "true" : "false")
               << ",\"items\":[";
        RationalTime cursor{0, 1};
        bool firstItem = true;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            // MOTION-2026-09 M4 -- a motion clip's source_in is a time of
            // its composition, at the composition's cadence.
            const std::optional<DocumentSource> source =
                ClipSource(document, clip);
            if (cursor < clip.timeline_in) {
                if (!firstItem) output << ',';
                output << "{\"type\":\"gap\",\"timeline_in\":";
                WriteTime(output, cursor, timelineRate);
                output << ",\"duration\":";
                WriteTime(output, clip.timeline_in.sub(cursor), timelineRate);
                output << '}';
                firstItem = false;
            }
            if (!firstItem) output << ',';
            output << "{\"type\":\""
                   << (track.kind == "caption" ? "caption" : "clip")
                   << "\",\"alias\":\"" << ClipAliasPrefix(trackOrdinal)
                   << (clipIndex + 1) << "\",\"id\":\"" << EscapeJson(clip.id)
                   << '"';
            if (track.kind != "caption") {
                if (IsMotionClip(clip)) {
                    output << ",\"composition_id\":\""
                           << EscapeJson(clip.composition_id) << '"';
                } else {
                    output << ",\"source_id\":\"" << EscapeJson(clip.source_id)
                           << '"';
                }
                output << ",\"source_in\":";
                WriteTime(output, clip.source_in,
                          source ? source->rate : timelineRate);
            }
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, clip.duration, timelineRate);
            if (track.kind == "caption")
                output << ",\"text\":\"" << EscapeJson(clip.caption_text)
                       << '"';
            else
                output << ",\"include_audio\":"
                       << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\""
                       << EscapeJson(clip.link_group_id) << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << EscapeJson(clip.sync_anchor_clip_id)
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta, timelineRate);
            }
            if (!clip.caption_group_id.empty())
                output << ",\"caption_group_id\":\""
                       << EscapeJson(clip.caption_group_id) << '"';
            if (track.kind != "caption" && !clip.caption_text.empty())
                output << ",\"caption_text\":\""
                       << EscapeJson(clip.caption_text) << '"';
            // PA4 -- a carton whose words are timed says how many, not
            // when: the times are what an animated style reads, and a
            // montage's worth of them would double this answer.
            if (!clip.caption_word_times.empty())
                output << ",\"timed_words\":" << clip.caption_word_times.size();
            if (!clip.motion_props.IsNull())
                output << ",\"motion_props\":" << clip.motion_props.Dump();
            // Out of lot -- the author, 29 October 2026: what set_clip_enabled
            // sets, written only on a disabled clip, as the file has it.
            if (!clip.enabled) output << ",\"enabled\":false";
            WriteClipSettings(output, clip);
            output << '}';
            firstItem = false;
            cursor = clip.timeline_in.add(clip.duration);
        }
        if (cursor < duration) {
            if (!firstItem) output << ',';
            output << "{\"type\":\"gap\",\"timeline_in\":";
            WriteTime(output, cursor, timelineRate);
            output << ",\"duration\":";
            WriteTime(output, duration.sub(cursor), timelineRate);
            output << '}';
        }
        output << "]}";
    }
    output << "],\"duration\":";
    WriteTime(output, duration, timelineRate);
    output << "},\"library\":[";
    std::set<Ulid> usedMedia;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            usedMedia.insert(clip.source_id);
        }
    }
    for (size_t index = 0; index < document.library.size(); ++index) {
        if (index) output << ',';
        const LibraryMedia& media = document.library[index];
        output << "{\"alias\":\"M" << (index + 1) << "\",\"id\":\""
               << EscapeJson(media.id) << "\",\"path\":\""
               << EscapeJson(media.path) << "\",\"filename\":\""
               << EscapeJson(media.filename) << "\"";
        if (media.metadata_complete) {
            output << ",\"codec\":\"" << EscapeJson(media.codec)
                   << "\",\"has_video\":"
                   << (media.has_video ? "true" : "false")
                   << ",\"width\":" << media.width
                   << ",\"height\":" << media.height
                   << ",\"rotation_degrees\":" << media.rotation_degrees
                   << ",\"pixel_format\":\"" << EscapeJson(media.pixel_format)
                   << "\",\"color_range\":\"" << EscapeJson(media.color_range)
                   << "\",\"color_space\":\"" << EscapeJson(media.color_space)
                   << "\",\"color_transfer\":\""
                   << EscapeJson(media.color_transfer)
                   << "\",\"color_primaries\":\""
                   << EscapeJson(media.color_primaries) << "\"";
        }
        if (media.white_balance_kelvin > 0)
            output << ",\"white_balance_kelvin\":"
                   << media.white_balance_kelvin;
        // G3 -- present only when the media does not follow the project.
        if (DeclaresColorInput(media.color_input)) {
            output << ",\"color_input\":{\"gamut\":\""
                   << EscapeJson(media.color_input.gamut)
                   << "\",\"transfer\":\""
                   << EscapeJson(media.color_input.transfer)
                   << "\",\"ycbcr_matrix\":\""
                   << EscapeJson(media.color_input.ycbcr_matrix)
                   << "\",\"range\":\"" << EscapeJson(media.color_input.range)
                   << "\"}";
        }
        // GN3 -- docs/roadmap/GENERATIF-2026-10.md. Present only on a media
        // a service generated: an agent placing it knows it is no rush.
        if (IsGenerated(media.generation)) {
            const MediaGeneration& generation = media.generation;
            output << ",\"generation\":{\"service\":\""
                   << EscapeJson(generation.service) << "\",\"model\":\""
                   << EscapeJson(generation.model) << "\",\"kind\":\""
                   << EscapeJson(generation.kind) << "\",\"prompt\":\""
                   << EscapeJson(generation.prompt) << "\",\"seed\":\""
                   << EscapeJson(generation.seed) << "\",\"created_at\":\""
                   << EscapeJson(generation.created_at)
                   << "\",\"announced_cost\":\""
                   << EscapeJson(generation.announced_cost)
                   << "\",\"source_media_id\":\""
                   << EscapeJson(generation.source_media_id) << "\"";
            // GN6b -- a voice the engine made says its reference words.
            if (!generation.reference_text.empty()) {
                output << ",\"reference_text\":\""
                       << EscapeJson(generation.reference_text) << "\"";
            }
            output << "}";
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den
               << "},\"duration\":{\"value\":" << media.duration.value
               << ",\"rate\":" << media.duration.rate << "}";
        if (media.metadata_complete) {
            output << ",\"orientation\":\"" << EscapeJson(media.orientation)
                   << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
                // QC-2026-09 A3 -- what tells a silent cutaway from speech.
                if (media.audio_level_measured)
                    output << ",\"audio_level\":" << media.audio_level;
            }
        }
        if (!media.bin_id.empty())
            output << ",\"bin_id\":\"" << EscapeJson(media.bin_id) << "\"";
        output << ",\"in_use\":"
               << (usedMedia.count(media.id) ? "true" : "false");
        // PL4 -- docs/roadmap/PLANS-2026-10.md. How many stretches of a
        // picture are described: 0 says the rush is still to describe once
        // (describe_shots), before search_shots can find anything in it.
        if (!media.metadata_complete || media.has_video)
            output << ",\"described_shots\":" << media.shots.size();
        output << '}';
    }
    output << "],\"bins\":[";
    for (size_t index = 0; index < document.bins.size(); ++index) {
        if (index) output << ',';
        output << "{\"id\":\"" << EscapeJson(document.bins[index].id)
               << "\",\"name\":\"" << EscapeJson(document.bins[index].name)
               << "\"";
        if (!document.bins[index].parent_id.empty())
            output << ",\"parent_id\":\""
                   << EscapeJson(document.bins[index].parent_id) << "\"";
        output << "}";
    }
    output << "],\"markers\":[";
    for (size_t index = 0; index < document.sequence.markers.size(); ++index) {
        if (index) output << ',';
        const DocumentMarker& marker = document.sequence.markers[index];
        output << "{\"alias\":\"K" << (index + 1) << "\",\"id\":\""
               << EscapeJson(marker.id) << "\",\"name\":\""
               << EscapeJson(marker.name) << "\",\"time\":";
        WriteTime(output, marker.time, timelineRate);
        // D6 -- a span says how long it is; a point says nothing, as before.
        if (marker.duration.value > 0) {
            output << ",\"duration\":";
            WriteTime(output, marker.duration, timelineRate);
        }
        output << ",\"color\":\"" << EscapeJson(marker.color)
               << "\",\"category\":\"" << EscapeJson(marker.category) << "\"}";
    }
    output << "],\"transitions\":[";
    for (size_t index = 0; index < document.sequence.transitions.size();
         ++index) {
        if (index) output << ',';
        const DocumentTransition& transition =
            document.sequence.transitions[index];
        output << "{\"id\":\"" << EscapeJson(transition.id)
               << "\",\"track_id\":\"" << EscapeJson(transition.track_id)
               << "\",\"left_clip_id\":\""
               << EscapeJson(transition.left_clip_id)
               << "\",\"right_clip_id\":\""
               << EscapeJson(transition.right_clip_id) << "\",\"type\":\""
               << EscapeJson(transition.type) << "\",\"duration\":";
        WriteExactTime(output, transition.duration);
        output << ",\"alignment\":\"" << AlignmentName(transition.alignment)
               << "\",\"color\":\"" << EscapeJson(transition.color) << "\"}";
    }
    output << "],\"caption_styles\":[";
    for (size_t index = 0; index < document.sequence.caption_styles.size();
         ++index) {
        if (index) output << ',';
        const CaptionStyle& style = document.sequence.caption_styles[index];
        output << "{\"id\":\"" << EscapeJson(style.id)
               << "\",\"font_family\":\"" << EscapeJson(style.font_family)
               << "\",\"font_size\":" << style.font_size
               << ",\"font_weight\":" << style.font_weight << ",\"color\":\""
               << EscapeJson(style.color) << "\",\"position\":\""
               << EscapeJson(style.position)
               << "\",\"outline_width\":" << style.outline_width
               << ",\"outline_color\":\"" << EscapeJson(style.outline_color)
               << "\",\"shadow_offset\":" << style.shadow_offset
               << ",\"shadow_color\":\"" << EscapeJson(style.shadow_color)
               << "\",\"background_color\":\""
               << EscapeJson(style.background_color)
               << "\",\"background_padding\":" << style.background_padding
               << ",\"vertical_position\":{\"num\":"
               << style.vertical_position.num
               << ",\"den\":" << style.vertical_position.den << "}";
        // PA4 -- said only on a style that animates its words.
        if (!style.word_animation.empty())
            output << ",\"word_animation\":\""
                   << EscapeJson(style.word_animation)
                   << "\",\"highlight_color\":\""
                   << EscapeJson(style.highlight_color) << '"';
        output << '}';
    }
    output << "],\"multicam_groups\":[";
    for (size_t index = 0; index < document.sequence.multicam_groups.size();
         ++index) {
        if (index) output << ',';
        const MulticamGroup& group = document.sequence.multicam_groups[index];
        output << "{\"id\":\"" << EscapeJson(group.id) << "\",\"name\":\""
               << EscapeJson(group.name) << "\",\"angles\":[";
        for (size_t angle = 0; angle < group.angles.size(); ++angle) {
            if (angle) output << ',';
            output << "{\"id\":\"" << EscapeJson(group.angles[angle].id)
                   << "\",\"name\":\"" << EscapeJson(group.angles[angle].name)
                   << "\",\"clip_id\":\""
                   << EscapeJson(group.angles[angle].clip_id) << "\"}";
        }
        output << ']';
        if (!group.active_angle_id.empty())
            output << ",\"active_angle_id\":\""
                   << EscapeJson(group.active_angle_id) << '"';
        output << '}';
    }
    // R6 -- docs/roadmap/REEL-2026-09.md. Written only when the sequence
    // carries one, like every other setting describe shows at its default
    // by omission: an agent that reads no `look_lut` reads a timeline with
    // no look. The three strings are spelled exactly as set_sequence_lut
    // reports them, digest included, so a reader can tell two looks apart.
    const SequenceLut& look = document.sequence.look_lut;
    if (!look.digest.empty()) {
        output << "],\"look_lut\":{\"digest\":\"" << EscapeJson(look.digest)
               << "\",\"name\":\"" << EscapeJson(look.name) << "\",\"space\":\""
               << EscapeJson(look.space) << "\"}";
    } else {
        output << ']';
    }
    const ColorManagementSettings& color = document.color_management;
    output << ",\"color_management\":{\"enabled\":"
           << (color.enabled ? "true" : "false") << ",\"input_gamut\":\""
           << EscapeJson(color.input_gamut) << "\",\"input_transfer\":\""
           << EscapeJson(color.input_transfer) << "\",\"input_ycbcr_matrix\":\""
           << EscapeJson(color.input_ycbcr_matrix) << "\",\"input_range\":\""
           << EscapeJson(color.input_range) << "\",\"working_gamut\":\""
           << EscapeJson(color.working_gamut) << "\",\"output_gamut\":\""
           << EscapeJson(color.output_gamut) << "\",\"output_transfer\":\""
           << EscapeJson(color.output_transfer) << "\"}}\n";
    return output.str();
}

// B8 -- ROADMAP.md. Keep project-level metadata structured while composing
// it with the existing document description; relying on a trailing `}\n`
// would make an unrelated formatting change silently corrupt describe.
mcp_json::Value DescribeTimelines(const Project& project) {
    mcp_json::Value timelines = mcp_json::Value::MakeArray();
    for (const DocumentSequence& sequence : project.timelines) {
        const RationalTime duration =
            Timeline(project.MakeDocument(sequence.id)).Duration();
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("id", mcp_json::Value::MakeString(sequence.id));
        item.Set("name", mcp_json::Value::MakeString(sequence.name));
        item.Set("width", mcp_json::Value::MakeInt(sequence.width));
        item.Set("height", mcp_json::Value::MakeInt(sequence.height));
        mcp_json::Value frameRate = mcp_json::Value::MakeObject();
        frameRate.Set("num", mcp_json::Value::MakeInt(sequence.frame_rate.num));
        frameRate.Set("den", mcp_json::Value::MakeInt(sequence.frame_rate.den));
        item.Set("frame_rate", std::move(frameRate));
        mcp_json::Value durationValue = mcp_json::Value::MakeObject();
        durationValue.Set(
            "frames", mcp_json::Value::MakeInt(duration.to_frames(
                          sequence.frame_rate.num, sequence.frame_rate.den)));
        durationValue.Set("seconds", mcp_json::Value::MakeRawNumber(
                                         DecimalSeconds(duration)));
        item.Set("duration", std::move(durationValue));
        item.Set("active", mcp_json::Value::MakeBool(
                               sequence.id == project.active_timeline_id));
        // BINS_SPEC.md -- where set_timeline_bin filed it; left out at the
        // project root, as a rush without a bin.
        if (const auto placement = project.timeline_bin_ids.find(sequence.id);
            placement != project.timeline_bin_ids.end()) {
            item.Set("bin_id", mcp_json::Value::MakeString(placement->second));
        }
        timelines.Push(std::move(item));
    }
    return timelines;
}

}  // namespace

// F2.4 -- ROADMAP.md chat panel. Exposes the same JSON view DescribeCommand
// serializes from a project file path, directly on an in-memory Document,
// for a backend (McpLiveBackend.h) that already holds the app's live
// document instead of a path to reload from disk. Same serialization, same
// aliasing scheme (A1/A2.../K1... the chat and MCP tool catalog both rely
// on) -- just skipping the project-file round trip DescribeCommand does.
std::string DescribeDocument(const Document& document) {
    return Describe(document);
}

std::string DescribeProject(const Project& project,
                            const std::string& timelineId) {
    mcp_json::Value description;
    std::string error;
    if (!mcp_json::Value::Parse(
            Describe(timelineId.empty() ? project.MakeActiveDocument()
                                        : project.MakeDocument(timelineId)),
            description, error)) {
        return "{\"ok\":false,\"error\":\"ParseError\",\"detail\":\"" +
               EscapeJson(error) + "\"}\n";
    }
    description.Set("timelines", DescribeTimelines(project));
    return description.Dump() + "\n";
}

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId) {
    return projectPath + ".timeline-" + timelineId + ".editlog.json";
}

std::string ProjectEditLogPathForProject(const std::string& projectPath) {
    return projectPath + ".project-editlog.json";
}

bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    return CommitArtifacts(std::move(prepared), message);
}

bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message) {
    if (!RecoverTransaction(std::filesystem::absolute(directory), message))
        return false;
    RemoveTransactionLeftovers(std::filesystem::absolute(directory));
    message.clear();
    return true;
}

bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size() + removals.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    for (const std::string& removal : removals) {
        CommitArtifact artifact;
        artifact.path = removal;
        artifact.remove = true;
        prepared.push_back(std::move(artifact));
    }
    return CommitArtifacts(std::move(prepared), message);
}

int CheckFfmpegCommand(std::string& output) {
    const ffmpeg::Diagnosis diagnosis = ffmpeg::Diagnose();
    if (!diagnosis.location.Found()) {
        return FailCliCommand(diagnosis.ErrorName(), diagnosis.Detail(), output,
                              1);
    }
    // DEC32 -- docs/roadmap/PRODUIT-2026-10.md. Found is enough to exit 0,
    // as for `--check-setup`: an FFmpeg without libx265 edits and exports.
    // It used to exit 1 for any missing row, so a script could not tell "no
    // FFmpeg" from "no HEVC". What it lacks stays in the answer, under the
    // names the `ffmpeg` item of `--check-setup` gives it.
    mcp_json::Value answer = mcp_json::Value::MakeObject();
    answer.Set("ok", mcp_json::Value::MakeBool(true));
    answer.Set("complete", mcp_json::Value::MakeBool(diagnosis.Complete()));
    // Held by name: a range-for does not keep a temporary alive under the
    // reference AsObject() returns.
    const mcp_json::Value report = ffmpeg::DiagnosisJson(diagnosis);
    for (const auto& member : report.AsObject()) {
        answer.Set(member.first, member.second);
    }
    if (!diagnosis.Complete()) {
        const mcp_json::Value problem = ffmpeg::DiagnosisProblemJson(diagnosis);
        for (const auto& member : problem.AsObject()) {
            if (member.first != "path") {
                answer.Set(member.first, member.second);
            }
        }
    }
    output = answer.Dump() + "\n";
    return 0;
}

int ProjectArchiveCommand(const std::vector<std::string>& arguments, bool query,
                          std::string& output) {
    if (arguments.size() < 2 || arguments.size() > 3) {
        return FailCliCommand("ValidationFailed",
                              "expected two arguments "
                              "and optional JSON options",
                              output, 2);
    }
    mcp_json::Value args = mcp_json::Value::MakeObject();
    std::string message;
    if (arguments.size() == 3 &&
        (!mcp_json::Value::Parse(arguments[2], args, message) ||
         !args.IsObject())) {
        return FailCliCommand("ValidationFailed",
                              "options must be a JSON "
                              "object: " +
                                  message,
                              output, 2);
    }
    if (args.Find("database") != nullptr || args.Find("sql") != nullptr) {
        return FailCliCommand("ValidationFailed",
                              "database and SQL are "
                              "positional arguments",
                              output, 2);
    }
    std::error_code pathError;
    const std::filesystem::path database =
        std::filesystem::absolute(arguments[query ? 0 : 1], pathError);
    if (pathError) {
        return FailCliCommand("IoError", pathError.message(), output, 1);
    }
    args.Set("database", mcp_json::Value::MakeString(database.string()));
    if (query) {
        args.Set("sql", mcp_json::Value::MakeString(arguments[1]));
    }
    const std::string projectPath =
        query ? "" : ProjectPathOfPackage(arguments[0]);
    ProjectSessionLock lock;
    if (!query && !LockProjectForCliWriter(projectPath, lock, output)) {
        return 1;
    }
    McpProjectBackend backend(projectPath);
    McpToolRegistry registry;
    const McpToolCallOutcome outcome = registry.Call(
        backend, query ? "query_project_archive" : "archive_project", args);
    if (!outcome.ok) {
        return FailCliCommand(outcome.error_name, outcome.message, output, 1);
    }
    output = outcome.result_json + "\n";
    return 0;
}

int RunStepCommand(const std::vector<std::string>& arguments, bool commit,
                   std::string& output) {
    if (arguments.size() != 2) {
        return FailCliCommand("ValidationFailed",
                              "expected a project and "
                              "a run request JSON or step ULID",
                              output, 2);
    }
    const std::string path = ProjectPathOfPackage(arguments[0]);
    ProjectSessionLock lock;
    if (!LockProjectForCliWriter(path, lock, output)) {
        return 1;
    }
    McpProjectBackend backend(path);
    McpToolRegistry registry;
    mcp_json::Value args = mcp_json::Value::MakeObject();
    args.Set(commit ? "request_json" : "step_id",
             mcp_json::Value::MakeString(arguments[1]));
    const auto result = registry.Call(
        backend, commit ? "commit_run_step" : "get_run_step", args);
    if (!result.ok) {
        return FailCliCommand(result.error_name, result.message, output, 1);
    }
    output = result.result_json + "\n";
    return 0;
}

int CheckSetupCommand(std::string& output) {
    const setup::Report report = setup::CheckSetup(setup::CurrentMachine());
    output = setup::ReportJson(report).Dump() + "\n";
    return report.Ok() ? 0 : 1;
}

int ProjectArchiveMaintenanceCommand(const std::vector<std::string>& arguments,
                                     const std::string& tool,
                                     std::string& output) {
    using mcp_json::Value;
    const bool importing = tool == "import_project_archive";
    // DEC44 -- the copy names no project: the archive and its folder are
    // the machine's.
    const bool copying = tool == "backup_project_archive";
    if ((importing && (arguments.size() < 2 || arguments.size() > 101)) ||
        (copying && !arguments.empty()) ||
        (!importing && !copying && arguments.size() != 1)) {
        return FailCliCommand("ValidationFailed", "invalid archive arguments",
                              output, 2);
    }
    Value args = Value::MakeObject();
    std::error_code pathError;
    std::string projectPath;
    if (importing) {
        const auto database =
            std::filesystem::absolute(arguments[0], pathError);
        if (pathError) {
            return FailCliCommand("IoError", pathError.message(), output, 1);
        }
        args.Set("database", Value::MakeString(database.string()));
        Value projects = Value::MakeArray();
        for (size_t i = 1; i < arguments.size(); ++i) {
            const auto path =
                std::filesystem::absolute(arguments[i], pathError);
            if (pathError) {
                return FailCliCommand("IoError", pathError.message(), output,
                                      1);
            }
            projects.Push(Value::MakeString(path.string()));
        }
        args.Set("projects", std::move(projects));
    } else if (!copying) {
        projectPath = ProjectPathOfPackage(arguments[0]);
    }
    ProjectSessionLock lock;
    if (!importing && !copying &&
        !LockProjectForCliWriter(projectPath, lock, output)) {
        return 1;
    }
    McpProjectBackend backend(projectPath);
    McpToolRegistry registry;
    const auto outcome = registry.Call(backend, tool, args);
    if (!outcome.ok) {
        return FailCliCommand(outcome.error_name, outcome.message, output, 1);
    }
    output = outcome.result_json + "\n";
    Value report;
    std::string parseError;
    if (tool != "project_archive_status" &&
        Value::Parse(outcome.result_json, report, parseError)) {
        const auto* complete = report.Find("complete");
        if (complete != nullptr && !complete->AsBool()) {
            return 1;
        }
    }
    return 0;
}

int VersionCommand(std::string& output) {
    const BuildInfo& build = CurrentBuild();
    output = "{\"ok\":true,\"version\":\"" + EscapeJson(build.version) +
             "\",\"commit\":\"" + EscapeJson(build.commit) +
             "\",\"dirty\":" + (build.dirty ? "true" : "false") + "}\n";
    return 0;
}

int ProcedureCommand(const std::vector<std::string>& arguments,
                     std::string& output) {
    // The refusals of the command line itself, worded as the table of
    // commands words them (CliCommands.h): an option, or a third argument.
    for (const std::string& argument : arguments) {
        if (argument.rfind("--", 0) == 0) {
            return FailCliCommand(
                "ValidationFailed",
                "unknown --procedure option '" + argument + "'", output, 2);
        }
    }
    if (arguments.size() > 2) {
        return FailCliCommand(
            "ValidationFailed",
            "unexpected --procedure argument '" + arguments[2] + "'", output,
            2);
    }
    mcp_json::Value answer;
    if (arguments.empty()) {
        answer = ProcedureListingJson();
    } else {
        std::string errorName;
        std::string message;
        if (!ProcedureFileJson(
                arguments[0],
                arguments.size() == 2 ? arguments[1] : kProcedureMainFile,
                answer, errorName, message)) {
            return FailCliCommand(errorName, message, output, 1);
        }
    }
    // The answer is an object with members: `ok` goes in front.
    output = "{\"ok\":true," + answer.Dump().substr(1) + "\n";
    return 0;
}

int TrainCaptionBreaksCommand(const std::vector<std::string>& arguments,
                              std::string& output) {
    const CliCommand* command = FindCliCommand("--train-caption-breaks");
    CaptionBreakTrainingOptions options;
    bool install = false;
    std::set<std::string> given;
    std::vector<std::string> corpus;
    std::vector<std::string> evaluation;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        std::error_code ignored;
        if (argument.rfind("--", 0) != 0) {
            corpus.push_back(
                std::filesystem::absolute(argument, ignored).string());
            continue;
        }
        if (argument == "--evaluate") {
            if (index + 1 >= arguments.size())
                return FailCliCommand(
                    "ValidationFailed",
                    CliOptionRefusal(*command, arguments, index), output, 2);
            evaluation.push_back(
                std::filesystem::absolute(arguments[++index], ignored)
                    .string());
            continue;
        }
        const bool takesValue = argument == "--folds" ||
                                argument == "--epochs" || argument == "--step";
        if ((argument != "--install" && !takesValue) ||
            !given.insert(argument).second ||
            (takesValue && index + 1 >= arguments.size()))
            return FailCliCommand("ValidationFailed",
                                  CliOptionRefusal(*command, arguments, index),
                                  output, 2);
        if (argument == "--install") {
            install = true;
            continue;
        }
        const std::string& text = arguments[++index];
        int64_t value = 0;
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || value < 1 ||
            value > 1000)
            return FailCliCommand("ValidationFailed",
                                  argument +
                                      " takes a whole number from 1 to "
                                      "1000, got '" +
                                      text + "'",
                                  output, 2);
        if (argument == "--folds")
            options.folds = static_cast<int32_t>(value);
        else if (argument == "--epochs")
            options.epochs = static_cast<int32_t>(value);
        else
            options.step = value;
    }
    if (corpus.empty())
        return FailCliCommand("ValidationFailed",
                              "--train-caption-breaks is missing <corpus>",
                              output, 2);
    std::string result;
    std::string errorName;
    std::string message;
    if (!RunCaptionBreakForge(corpus, evaluation, options, install, result,
                              errorName, message))
        return FailCliCommand(errorName, message, output, 1);
    output = "{\"ok\":true," + result.substr(1) + "\n";
    return 0;
}

// PRD15 -- ROADMAP.md. The same listing as the server's `tools/list`
// (McpTools.h), annotations included: the two used to be written apart.
int ListToolsCommand(std::string& output, const McpToolProfile& profile) {
    const McpToolRegistry registry(profile);
    output = "{\"ok\":true,\"tools\":" + McpToolsListingJson(registry.Tools()) +
             "}\n";
    return 0;
}

int CallToolCommand(const std::string& projectPath, const std::string& toolName,
                    const std::string& argumentsJson, std::string& output,
                    bool requireExplicitTimeline) {
    mcp_json::Value arguments;
    std::string error;
    if (!mcp_json::Value::Parse(argumentsJson, arguments, error)) {
        return FailCliCommand("ParseError", error, output, 2);
    }
    // PL3 -- ROADMAP.md. A plan lives in the session that prepared it, and
    // this process is a session of its own: a plan_id handed out here would
    // name a plan nobody keeps, a promise broken in silence. The file-backed
    // commands keep it where their caller says.
    if (toolName == "prepare_edit_plan" || toolName == "apply_edit_plan" ||
        toolName == "discard_edit_plan") {
        return FailCliCommand(
            "UnsupportedOperation",
            "'" + toolName +
                "' needs a session that keeps the plan between calls: use "
                "--prepare-edit-plan, --apply-edit-plan and "
                "--discard-edit-plan, which keep it in a file you name, or "
                "an MCP server (--mcp-serve)",
            output);
    }
    // Like --mcp-serve, hold the project lock through snapshot, validation and
    // commit. A file-backed call must never race the resident app's document.
    // The backend owns the lock (IA7), so a create_project or open_project
    // call moves it rather than stacking a second one.
    McpProjectBackend backend({}, requireExplicitTimeline);
    std::string opened;
    std::string openError;
    if (!projectPath.empty() &&
        !backend.OpenProject(
            std::filesystem::absolute(projectPath).lexically_normal().string(),
            opened, openError, error)) {
        return FailCliCommand(openError, error, output);
    }
    const McpToolRegistry registry;
    const McpToolCallOutcome outcome =
        registry.Call(backend, toolName, arguments);
    if (!outcome.ok) {
        output = outcome.result_json + '\n';
        return 1;
    }
    mcp_json::Value result;
    if (!mcp_json::Value::Parse(outcome.result_json, result, error)) {
        return FailCliCommand("IoError", "invalid tool result: " + error,
                              output);
    }
    output = "{\"ok\":true,\"tool\":\"" + EscapeJson(toolName) +
             "\",\"result\":" + result.Dump();
    if (!outcome.image_base64.empty()) {
        output += ",\"image\":{\"mime\":\"" + EscapeJson(outcome.image_mime) +
                  "\",\"base64\":\"" + EscapeJson(outcome.image_base64) + "\"}";
    }
    output += "}\n";
    return 0;
}

bool CliCommandWritesProject(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return false;
    }
    static const std::vector<std::string> kWriters = {
        "--apply-op", "--apply-project-op", "--undo-project-op",
        "--redo-project-op", "--remove-words", "--tighten-pauses",
        "--trim-boundary-air", "--close-junction-air", "--import-resolve",
        "--transcribe", "--transcribe-timeline", "--shot-quality",
        "--speech-onset",
        // SYNCHRO-2026-10 SY2.
        "--sync-audio", "--sync-audio-clip", "--import-resolve-timeline",
        "--align-words"};
    const std::string& command = arguments[0];
    if (std::find(kWriters.begin(), kWriters.end(), command) !=
        kWriters.end()) {
        return true;
    }
    // Reports unless asked to install the corrected boundaries.
    if (command == "--align-transcripts") {
        return std::find(arguments.begin(), arguments.end(), "--write") !=
               arguments.end();
    }
    // R8 -- likewise: it reads the caches unless asked to measure, and only
    // then writes one.
    if (command == "--isolated-frames") {
        return std::find(arguments.begin(), arguments.end(), "--analyze") !=
               arguments.end();
    }
    return false;
}

std::string ProjectPathOfPackage(const std::string& path) {
    std::error_code error;
    const std::filesystem::path folder(path);
    if (!std::filesystem::is_directory(folder, error)) return path;
    const std::filesystem::path project = folder / "project.cutmachine.json";
    if (!std::filesystem::is_regular_file(project, error) ||
        !std::filesystem::is_regular_file(folder / "manifest.json", error))
        return path;
    return project.lexically_normal().string();
}

bool LockProjectForCliWriter(const std::string& projectPath,
                             ProjectSessionLock& lock, std::string& output) {
    std::error_code existsError;
    if (!std::filesystem::is_regular_file(projectPath, existsError)) {
        return true;
    }
    std::string owner;
    std::string error;
    if (lock.Acquire(projectPath, owner, error)) {
        return true;
    }
    FailCliCommand(
        "IoError",
        "this command writes the project and cannot lock it: " + error +
            "; a server or the app holding it applies its own "
            "edits -- go through it",
        output);
    return false;
}

int LockedIngestCommand(const std::string& projectPath,
                        const std::string& mediaPath, bool recursive,
                        std::string& output,
                        const std::optional<Ulid>& targetBin) {
    // IA7 -- ROADMAP.md. An MCP server keeps the project it holds loaded on
    // the assumption that nobody else writes it (PERF5); an ingest run beside
    // it left that copy stale. Same lock as --call-tool, so the ingest waits
    // its turn behind a server or the app instead of racing them.
    // PRD20 -- a project that is not there is said to be missing before a
    // lock is attempted beside it.
    if (IsMissingProjectPath(projectPath)) {
        return FailCliCommand("IoError", MissingProjectMessage(projectPath),
                              output);
    }
    ProjectSessionLock projectLock;
    std::string owner;
    std::string error;
    if (!projectLock.Acquire(projectPath, owner, error))
        return FailCliCommand(
            "IoError", "ingest cannot lock the project: " + error, output);
    return IngestCommand(projectPath, mediaPath, recursive, output, targetBin);
}

namespace {

// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. --export-srt and --export-vtt
// are one command with two serializers: the same cues, the same report.
int ExportSubtitleFileCommand(const std::string& projectPath,
                              const std::string& outputPath,
                              std::string& output,
                              const std::string& timelineId, bool wordByWord,
                              bool webVtt) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selectedTimeline + "'",
                              output);
    }
    Document document = project.MakeDocument(selectedTimeline);
    const std::filesystem::path transcriptDirectory =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "transcripts";
    std::vector<TimelineTranscriptSpan> spans;
    TimelineWordCheck check;
    const bool built =
        wordByWord ? BuildTimelineTranscriptWordSpans(
                         document, transcriptDirectory, spans, error, &check)
                   : BuildTimelineTranscriptSpans(document, transcriptDirectory,
                                                  spans, error, &check);
    if (!built) {
        return FailCliCommand("InvalidTranscript", error, output);
    }
    // Both builders already carry timeline positions: readable mode groups
    // words, while review mode deliberately keeps one stored boundary per
    // cue. Nothing here recomputes either kind of boundary.
    std::vector<SubtitleCue> cues;
    cues.reserve(spans.size());
    for (const TimelineTranscriptSpan& span : spans)
        cues.push_back({span.timeline_in, span.duration, span.text});
    size_t rounded = 0;
    if (!(webVtt ? WriteVtt(cues, outputPath, error, &rounded)
                 : WriteSrt(cues, outputPath, error, &rounded))) {
        return FailCliCommand("InvalidSubtitles", error, output);
    }
    output = std::string("{\"ok\":true,") +
             (webVtt ? "\"format\":\"webvtt\"," : "") +
             "\"cues\":" + std::to_string(cues.size()) +
             ",\"rounded_timestamps\":" + std::to_string(rounded) +
             ",\"word_by_word\":" + (wordByWord ? "true" : "false") +
             ",\"segmentation\":\"" +
             (wordByWord ? "word_boundaries" : "syntax_and_timing") +
             "\",\"target_cps\":" +
             (wordByWord ? "null"
                         : std::to_string(kSubtitleTargetCharactersPerSecond)) +
             ",\"path\":\"" + EscapeJson(outputPath) + '"';
    // ITW3 -- what the cues were built from, and which words were left out
    // because no reading of the montage audio contains them.
    mcp_json::Value unheard = mcp_json::Value::MakeArray();
    for (const UnheardWord& word : check.unheard) {
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("text", mcp_json::Value::MakeString(word.text));
        item.Set("clip_id", mcp_json::Value::MakeString(word.clip_id));
        mcp_json::Value position = mcp_json::Value::MakeObject();
        position.Set("value", mcp_json::Value::MakeInt(word.timeline_in.value));
        position.Set("rate", mcp_json::Value::MakeInt(word.timeline_in.rate));
        item.Set("timeline_in", std::move(position));
        unheard.Push(std::move(item));
    }
    mcp_json::Value timing = mcp_json::Value::MakeObject();
    for (const auto& entry : check.word_timing)
        timing.Set(entry.first, mcp_json::Value::MakeString(entry.second));
    output += ",\"word_timing\":" + timing.Dump() +
              ",\"montage_readings\":" + std::to_string(check.readings) +
              ",\"unheard_words\":" + unheard.Dump();
    // AGENT-2026-09 A38 -- a check that could not run says so: on the third
    // run `montage_readings: 0` after a retouch read as "nothing unheard",
    // and a third transcribe_timeline pass was the only way to know.
    if (check.readings == 0)
        output +=
            ",\"word_check_skipped\":\"no reading of the montage's "
            "sound at this revision: unheard_words is empty because "
            "nothing was checked, not because every word was heard; "
            "transcribe_timeline reads it, list_timeline_words gives "
            "the aligned words each clip covers without it\"";
    // BENCH-2026-09 -- words kept through an inflection, words heard
    // although aligned into a cut, and cue gaps while a word is said: the
    // same report as generate_captions, so no one diffs the cues by hand.
    mcp_json::Value wordCheck;
    std::string parseError;
    if (mcp_json::Value::Parse(SerializeTimelineWordCheck(check), wordCheck,
                               parseError)) {
        for (const char* key : {"inflected_matches", "cut_but_heard"}) {
            if (const mcp_json::Value* list = wordCheck.Find(key)) {
                output += std::string(",\"") + key + "\":" + list->Dump();
            }
        }
    }
    std::vector<std::pair<RationalTime, RationalTime>> shown;
    shown.reserve(cues.size());
    for (const SubtitleCue& cue : cues) {
        shown.emplace_back(cue.timeline_in, cue.duration);
    }
    output += ",\"caption_gaps\":" +
              SerializeCaptionSpeechGaps(FindCaptionSpeechGaps(shown, check)) +
              "}\n";
    return 0;
}

}  // namespace

int ExportSrtCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output,
                     const std::string& timelineId, bool wordByWord) {
    return ExportSubtitleFileCommand(projectPath, outputPath, output,
                                     timelineId, wordByWord, false);
}

int ExportVttCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output,
                     const std::string& timelineId, bool wordByWord) {
    return ExportSubtitleFileCommand(projectPath, outputPath, output,
                                     timelineId, wordByWord, true);
}

namespace {

// The settings of one export, made once the timeline it exports is read.
using ExportSettingsChooser = std::function<ExportSettings(const Document&)>;

// One load, one plan and one run, whichever way the export was named: the
// timeline `timelineId` names -- the active one when it is empty -- of the
// project at `documentPath`, with the settings `choose` makes for it.
int RunExportCommand(const std::string& documentPath,
                     const ExportSettingsChooser& choose,
                     const ExportProgressCallback& progress,
                     const std::atomic_bool* cancel, std::string& output,
                     const std::string& timelineId) {
    Project project;
    std::string error;
    if (!LoadStoredProject(documentPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selectedTimeline + "'",
                              output);
    }
    Document document = project.MakeDocument(selectedTimeline);
    const ExportSettings resolvedSettings = choose(document);
    ExportPlan plan;
    if (!Exporter::BuildPlan(document, documentPath, resolvedSettings, plan,
                             error)) {
        return FailCliCommand("InvalidExport", error, output);
    }
    if (!Exporter::Run(plan, progress, cancel, error)) {
        return FailCliCommand("ExportFailed", error, output);
    }
    output = Exporter::ResultJson(plan) + "\n";
    return 0;
}

}  // namespace

int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output,
                  const std::string& timelineId) {
    const ExportSettingsChooser deferred = [&](const Document& document) {
        ExportSettings resolvedSettings = settings;
        if (settings.width == 0 && settings.height == 0) {
            if (!Exporter::IsH264(settings.encoder)) {
                resolvedSettings = Exporter::SettingsForPreset(
                    ExportPresetId::HevcHighQuality, settings.output_path,
                    document.sequence.width, document.sequence.height,
                    document.sequence.frame_rate);
                resolvedSettings.encoder = settings.encoder;
                resolvedSettings.overwrite = settings.overwrite;
                resolvedSettings.ffmpeg_path = settings.ffmpeg_path;
                resolvedSettings.sdr_tag = settings.sdr_tag;
                resolvedSettings.loudness = settings.loudness;
            } else {
                resolvedSettings.width = document.sequence.width;
                resolvedSettings.height = document.sequence.height;
                resolvedSettings.frame_rate = document.sequence.frame_rate;
            }
        }
        return resolvedSettings;
    };
    return RunExportCommand(documentPath, deferred, progress, cancel, output,
                            timelineId);
}

int ExportCommand(const std::string& documentPath,
                  const std::string& outputPath,
                  const ExportCommandChoice& choice,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output,
                  const std::string& timelineId) {
    if (!choice.codec.empty() && choice.codec != "h264" &&
        choice.codec != "hevc") {
        return FailCliCommand("ValidationFailed",
                              "export codec must be h264 or hevc", output);
    }
    ExportPresetId preset = ExportPresetId::H264;
    std::string error;
    if (!choice.preset.empty() &&
        !Exporter::ReadPreset(choice.preset, choice.codec, "--preset", preset,
                              error)) {
        return FailCliCommand("ValidationFailed", error, output);
    }
    const auto finish = [&](ExportSettings settings) {
        if (choice.software) {
            Exporter::UseSoftwareEncoder(settings);
        }
        settings.overwrite = choice.overwrite;
        settings.loudness = choice.loudness;
        if (choice.rec709a) {
            settings.sdr_tag = ExportSdrTag::Rec709A;
        }
        return settings;
    };
    if (choice.preset.empty() && !choice.codec.empty()) {
        // A codec named alone: the settings --export has always made for it,
        // byte for byte.
        return ExportCommand(
            documentPath,
            finish(choice.codec == "h264"
                       ? Exporter::H264Preset(outputPath)
                       : Exporter::HevcMain10Preset(outputPath)),
            progress, cancel, output, timelineId);
    }
    const bool named = !choice.preset.empty();
    return RunExportCommand(
        documentPath,
        [&](const Document& document) {
            return finish(
                named
                    ? Exporter::SettingsForPreset(preset, document, outputPath)
                    : Exporter::DefaultSettings(document, outputPath));
        },
        progress, cancel, output, timelineId);
}

int CreateProjectCommand(const std::string& packagePath,
                         const std::string& projectName, std::string& output,
                         const std::string& format) {
    if (projectName.empty()) {
        output = ErrorJson(EditError::InvalidOperation,
                           "project name must not be empty");
        return 1;
    }
    Project project(projectName);
    DocumentSequence& timeline = project.timelines.front();
    if (!format.empty()) {
        // Digits only, so a sign, a space or a decimal point is refused
        // rather than read by a lenient number parser.
        const auto number = [](const std::string& text, int64_t maximum,
                               int32_t& value) {
            if (text.empty() || text.size() > 6 ||
                !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                }))
                return false;
            const int64_t parsed = std::stoll(text);
            if (parsed <= 0 || parsed > maximum) return false;
            value = static_cast<int32_t>(parsed);
            return true;
        };
        const size_t times = format.find('x');
        const size_t at = format.find('@');
        const std::string rate =
            at == std::string::npos ? "" : format.substr(at + 1);
        const size_t slash = rate.find('/');
        int32_t width = 0;
        int32_t height = 0;
        MediaRate frameRate{0, 1};
        if (times == std::string::npos || at == std::string::npos ||
            at < times || !number(format.substr(0, times), 16384, width) ||
            !number(format.substr(times + 1, at - times - 1), 16384, height) ||
            !number(rate.substr(0, slash), 240000, frameRate.num) ||
            (slash != std::string::npos &&
             !number(rate.substr(slash + 1), 10000, frameRate.den))) {
            output = ErrorJson(EditError::InvalidOperation,
                               "project format must be WIDTHxHEIGHT@RATE, "
                               "e.g. 1080x1920@25 or 3840x2160@24000/1001");
            return 1;
        }
        timeline.width = width;
        timeline.height = height;
        timeline.frame_rate = frameRate;
    }
    std::string projectPath;
    std::string error;
    if (!CreatePortableProject(packagePath, project, projectPath, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    // IA10 -- the tracks are named so a first insert can target them without
    // a describe round trip. PRD14 -- by their alias too, in describe's
    // order, so that insert can be written without copying a ULID.
    std::string tracks;
    const std::vector<const DocumentTrack*> listed =
        TracksInDescribeOrder(timeline);
    for (size_t ordinal = 0; ordinal < listed.size(); ++ordinal) {
        const DocumentTrack& track = *listed[ordinal];
        if (!tracks.empty()) tracks += ',';
        tracks += '{' + TrackAliasMember(listed, ordinal) + "\"id\":\"" +
                  track.id + "\",\"kind\":\"" + track.kind +
                  "\",\"index\":" + std::to_string(track.index) + '}';
    }
    output =
        "{\"ok\":true,\"path\":\"" + EscapeJson(projectPath) +
        "\",\"timeline_id\":\"" + timeline.id +
        "\",\"width\":" + std::to_string(timeline.width) +
        ",\"height\":" + std::to_string(timeline.height) +
        ",\"frame_rate\":{\"num\":" + std::to_string(timeline.frame_rate.num) +
        ",\"den\":" + std::to_string(timeline.frame_rate.den) +
        "},\"tracks\":[" + tracks + "]}\n";
    return 0;
}

namespace {

// CHAT-PARITE -- F2.4. The one place a transcription command decides where
// its Whisper work runs: on the runner it was handed, or on a manager of its
// own that it waits on, as the CLI always did.
MediaTaskSnapshot RunCommandMediaTask(const MediaTaskRunner& runner,
                                      MediaTaskKind kind, std::string label,
                                      MediaTaskManager::Work work) {
    if (runner) {
        return runner(kind, std::move(label), std::move(work));
    }
    MediaTaskManager tasks(1);
    return RunMediaTaskAndWait(tasks, kind, std::move(label), std::move(work));
}

// Why a task that did not succeed did not. A cancelled task has no error of
// its own -- someone stopped it from the task bar -- unless the wait limit
// did ("timed out").
std::string MediaTaskFailure(const MediaTaskSnapshot& task,
                             const std::string& what) {
    if (task.state == MediaTaskState::Failed) {
        return task.error;
    }
    return what + ' ' + (task.error.empty() ? "cancelled" : task.error);
}

}  // namespace

int TranscribeTimelineCommand(const std::string& projectPath,
                              const std::string& timelineId,
                              const std::string& language, bool verbatim,
                              std::string& output,
                              const MediaTaskRunner& runner,
                              const Project* stored,
                              WhisperDecodingProfile profile) {
    std::string profileError;
    if (!ValidateWhisperDecodingRequest(profile, language, verbatim,
                                        profileError))
        return FailCliCommand("ValidationFailed", profileError, output);
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selected + "'", output);
    }
    std::string modelPath;
    if (!ResolveConfiguredWhisperModel(modelPath, error))
        return FailCliCommand("IoError", error, output);
    WhisperSettings settings;
    settings.whisper_model_path = modelPath;
    settings.language = language.empty() ? "auto" : language;
    settings.verbatim = verbatim;
    settings.decoding_profile = profile;
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    const std::filesystem::path cache = TimelineTranscriptCachePath(
        base / ".cutmachine", selected, verbatim, profile);
    Transcript transcript;
    bool cacheHit = false;
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::Transcription, "Whisper timeline",
        [&](MediaTaskContext& context, std::string& taskError) {
            return TranscribeTimelineAudio(
                project.MakeDocument(selected), projectPath, cache, settings,
                context, transcript, cacheHit, taskError);
        });
    if (task.state != MediaTaskState::Succeeded) {
        return FailCliCommand("IoError",
                              MediaTaskFailure(task, "timeline transcription"),
                              output);
    }
    std::ostringstream text;
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        if (index) text << ' ';
        text << transcript.words[index].text;
    }
    output = "{\"ok\":true,\"timeline_id\":\"" + EscapeJson(selected) +
             "\",\"decoding_profile\":\"" +
             WhisperDecodingProfileName(transcript.decoding_profile) +
             "\",\"cached\":" + (cacheHit ? "true" : "false") +
             ",\"words\":" + std::to_string(transcript.words.size()) +
             ",\"text\":\"" + EscapeJson(text.str()) + "\"}\n";
    return 0;
}

int SrtFromMediaCommand(const std::string& mediaPath,
                        const std::string& outputPath,
                        const std::string& language, bool verbatim,
                        std::string& output, WhisperDecodingProfile profile) {
    std::string profileError;
    if (!ValidateWhisperDecodingRequest(profile, language, verbatim,
                                        profileError))
        return FailCliCommand("ValidationFailed", profileError, output);
    if (!std::filesystem::is_regular_file(mediaPath)) {
        return FailCliCommand(
            "IoError", "media is not a regular file: '" + mediaPath + "'",
            output);
    }
    LibraryMedia media;
    std::string error;
    if (!ProbeMediaMetadata(mediaPath, media, error) || !media.has_audio) {
        return FailCliCommand(
            "InvalidOperation",
            error.empty() ? "media has no audio stream" : error, output);
    }
    std::string modelPath;
    if (!ResolveConfiguredWhisperModel(modelPath, error))
        return FailCliCommand("IoError", error, output);
    WhisperSettings settings;
    settings.whisper_model_path = modelPath;
    settings.language = language.empty() ? "auto" : language;
    settings.verbatim = verbatim;
    settings.decoding_profile = profile;
    const std::filesystem::path cache =
        std::filesystem::temp_directory_path() /
        ("cutmachine-srt-" + GenerateUlid() + ".json");
    const Ulid mediaId = "media:" + GenerateUlid();
    Transcript transcript;
    MediaTaskManager tasks(1);
    const Ulid task =
        tasks.Enqueue(MediaTaskKind::Transcription, "Whisper SRT",
                      [&](MediaTaskContext& context, std::string& taskError) {
                          return GenerateAudioTranscript(
                              mediaPath, cache.string(), mediaId, media.rate,
                              settings, context, taskError);
                      });
    const auto removeCache = [&]() {
        std::error_code ignored;
        std::filesystem::remove(cache, ignored);
    };
    if (!tasks.WaitForIdle(24 * 60 * 60 * 1000)) {
        tasks.Cancel(task);
        removeCache();
        return FailCliCommand("IoError", "media transcription timed out",
                              output);
    }
    const std::vector<MediaTaskSnapshot> snapshots = tasks.Snapshot();
    const auto snapshot = std::find_if(
        snapshots.begin(), snapshots.end(),
        [&](const MediaTaskSnapshot& item) { return item.id == task; });
    if (snapshot == snapshots.end() ||
        snapshot->state != MediaTaskState::Succeeded) {
        const std::string detail = snapshot == snapshots.end()
                                       ? "media transcription task disappeared"
                                       : snapshot->error;
        removeCache();
        return FailCliCommand("IoError", detail, output);
    }
    if (!LoadAudioTranscript(cache.string(), transcript, error)) {
        removeCache();
        return FailCliCommand("IoError", error, output);
    }
    DocumentClip wholeMedia;
    wholeMedia.source_id = mediaId;
    wholeMedia.source_in = {0, media.rate.num};
    wholeMedia.timeline_in = {0, media.rate.num};
    wholeMedia.duration = media.duration;
    std::vector<SubtitleCue> cues;
    size_t rounded = 0;
    if (!SubtitleCuesForClip(transcript, wholeMedia, cues, error) ||
        !WriteSrt(cues, outputPath, error, &rounded)) {
        removeCache();
        return FailCliCommand("InvalidSubtitles", error, output);
    }
    removeCache();
    output = "{\"ok\":true,\"cues\":" + std::to_string(cues.size()) +
             ",\"rounded_timestamps\":" + std::to_string(rounded) +
             ",\"path\":\"" + EscapeJson(outputPath) + "\"}\n";
    return 0;
}

int TranscribeMediaCommand(const std::string& projectPath,
                           const std::vector<std::string>& mediaIds,
                           const std::string& whisperModelPath,
                           const std::string& language, bool verbatim,
                           bool includeSilent, std::string& output,
                           const MediaTaskRunner& runner, const Project* stored,
                           bool overwrite, WhisperDecodingProfile profile) {
    std::string profileError;
    if (!ValidateWhisperDecodingRequest(profile, language, verbatim,
                                        profileError))
        return FailCliCommand("ValidationFailed", profileError, output);
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    if (mediaIds.empty()) {
        output = ErrorJson(EditError::InvalidOperation,
                           "at least one media_id is required");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    struct Skipped {
        std::string media_id;
        std::string reason;
    };
    std::vector<TranscriptionJob> jobs;
    std::vector<Skipped> skipped;
    std::map<std::string, std::filesystem::path> staged;
    // Whatever happens, no staged transcript outlives the command.
    struct StagedCleanup {
        const std::map<std::string, std::filesystem::path>& paths;
        ~StagedCleanup() {
            std::error_code ignored;
            for (const auto& entry : paths) {
                std::filesystem::remove(entry.second, ignored);
            }
        }
    } stagedCleanup{staged};
    for (const std::string& mediaId : mediaIds) {
        const auto media = std::find_if(
            project.rushes.begin(), project.rushes.end(),
            [&](const LibraryMedia& item) { return item.id == mediaId; });
        const auto source = std::find_if(
            project.sources.begin(), project.sources.end(),
            [&](const DocumentSource& item) { return item.id == mediaId; });
        if (media == project.rushes.end() || source == project.sources.end()) {
            output = ErrorJson(EditError::UnknownMedia,
                               "unknown media_id '" + mediaId + "'");
            return 1;
        }
        if (!media->has_audio) {
            output = ErrorJson(EditError::InvalidOperation,
                               "media '" + mediaId + "' has no audio stream");
            return 1;
        }
        // QC-2026-09 A3 -- a mute cutaway costs eleven times its own runtime
        // to produce nothing. 29 of one project's 71 rushes were exactly
        // that, at -74 dBFS. Skipping them is not a guess: the level was
        // measured at ingest and is a document fact. `includeSilent` exists
        // because a threshold is still a threshold, and a caller who
        // disagrees must be able to say so.
        if (!includeSilent && media->audio_level_measured &&
            media->audio_level < kSilentMediaAudioLevel) {
            skipped.push_back(
                {mediaId, "measured audio level " +
                              std::to_string(media->audio_level) +
                              " is below the silence threshold "
                              "(" +
                              std::to_string(kSilentMediaAudioLevel) + ")"});
            continue;
        }
        std::filesystem::path input(media->path);
        if (input.is_relative()) input = base / input;
        input = input.lexically_normal();
        // PRD40 -- Whisper writes beside the cache, never over it: the new
        // transcript is installed below once it is known not to be worse
        // than the one every other reader of the project relies on.
        const std::filesystem::path staging =
            std::filesystem::temp_directory_path() /
            ("cutmachine-transcript-" + mediaId + "-" + GenerateUlid() +
             ".json");
        staged[mediaId] = staging;
        jobs.push_back(
            {mediaId, input.string(), staging.string(), source->rate});
    }

    // The model is resolved only once there is something to run it on: a
    // batch that turned out to be all mute cutaways needs no model, and
    // failing it for a missing one would be a wrong answer. Resolved after
    // the media loop for the same reason it used to come after the single
    // media lookup -- "you named a media that is not in this project" is a
    // more useful first answer than "the ggml file is missing".
    std::string modelPath = whisperModelPath;
    if (!jobs.empty()) {
        // An empty path means "use the configured model" -- the only form the
        // agent can use, since it has no way to know where a human keeps a
        // ggml file. An explicit path still wins, so the CLI stays scriptable.
        if (modelPath.empty() &&
            !ResolveConfiguredWhisperModel(modelPath, error)) {
            output = ErrorJson(EditError::IoError, error);
            return 1;
        }
        if (!std::filesystem::is_regular_file(modelPath)) {
            output = ErrorJson(
                EditError::IoError,
                "Whisper model is not a regular file: '" + modelPath + "'");
            return 1;
        }
    }

    std::vector<TranscriptionOutcome> outcomes;
    WhisperSettings settings;
    settings.whisper_model_path = modelPath;
    settings.language = language.empty() ? "auto" : language;
    settings.verbatim = verbatim;
    settings.decoding_profile = profile;
    if (!jobs.empty()) {
        std::string batchError;
        const MediaTaskSnapshot task = RunCommandMediaTask(
            runner, MediaTaskKind::Transcription,
            "Whisper " + std::to_string(jobs.size()) + " media",
            [&jobs, &outcomes, &batchError, settings](MediaTaskContext& context,
                                                      std::string& taskError) {
                const bool started = GenerateAudioTranscripts(
                    jobs, settings, context, outcomes, batchError);
                if (!started) taskError = batchError;
                return started;
            });
        if (task.state != MediaTaskState::Succeeded) {
            output = ErrorJson(EditError::IoError,
                               MediaTaskFailure(task, "transcription"));
            return 1;
        }
    }

    // What each media's passes made, until it is installed.
    struct Made {
        Transcript transcript;
        bool has_envelope = false;
        SpeechOnsetReport envelope;
    };
    std::map<std::string, Made> made;
    // AGENT-2026-09 A27 -- the text itself, bounded (a rush of an hour is
    // read with list_source_words), with or without an envelope: on the
    // fourth run the rushes were transcribed before they were analysed and
    // the text stayed out of the reply.
    const auto describeMade = [](TranscriptionOutcome& outcome,
                                 const Transcript& transcript) {
        constexpr size_t kMostTextBytes = 6000;
        outcome.words = transcript.words.size();
        outcome.detected_language = transcript.detected_language;
        outcome.text.clear();
        outcome.text_truncated = false;
        for (const TranscriptWord& word : transcript.words) {
            if (outcome.text.size() + word.text.size() + 1 > kMostTextBytes) {
                outcome.text_truncated = true;
                break;
            }
            if (!outcome.text.empty()) outcome.text += ' ';
            outcome.text += word.text;
        }
    };
    const auto assessMade = [](TranscriptionOutcome& outcome,
                               Transcript& transcript,
                               const SpeechOnsetReport& envelope,
                               std::string& error) {
        TranscriptSpeechAssessment assessment;
        if (transcript.words.empty() ||
            !AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                           error)) {
            return false;
        }
        ApplyTranscriptSpeechAssessment(transcript, assessment);
        outcome.speech_assessed = true;
        outcome.measured_speech_duration = assessment.measured_speech_duration;
        outcome.likely_hallucinated = assessment.likely_hallucinated;
        outcome.likely_incomplete = assessment.likely_incomplete;
        outcome.known_hallucination_phrase =
            assessment.known_hallucination_phrase;
        return true;
    };

    // B12 -- pair whichever artifact arrived second. Speech analysis and
    // transcription are independent, expensive commands, so neither forces
    // the other to run; when a B1 envelope is already cached, this pass makes
    // the transcript's safety marker part of the same successful command.
    for (TranscriptionOutcome& outcome : outcomes) {
        if (!outcome.ok) continue;
        const std::filesystem::path envelopePath =
            base / ".cutmachine" / "speechonset" / (outcome.media_id + ".json");
        Made item;
        std::string assessmentError;
        const auto stagedPath = staged.find(outcome.media_id);
        if (stagedPath == staged.end() ||
            !LoadAudioTranscript(stagedPath->second.string(), item.transcript,
                                 assessmentError)) {
            outcome.ok = false;
            outcome.error =
                "cannot read the new transcript: " + assessmentError;
            continue;
        }
        describeMade(outcome, item.transcript);
        std::error_code existsError;
        if (std::filesystem::exists(envelopePath, existsError) &&
            !existsError) {
            item.has_envelope = LoadSpeechOnset(envelopePath.string(),
                                                item.envelope, assessmentError);
            if (!item.has_envelope ||
                !assessMade(outcome, item.transcript, item.envelope,
                            assessmentError)) {
                // Installed unassessed, as it was before PRD40, and said.
                item.has_envelope = false;
                outcome.ok = false;
                outcome.error = "cannot assess transcript against speech: " +
                                assessmentError;
            }
        }
        made[outcome.media_id] = std::move(item);
    }

    // ITW9 -- docs/roadmap/ITW-2026-09.md. A verbatim pass that the speech
    // envelope flags likely_incomplete is done again without the verbatim
    // prompt, on those media only: on PRJK, four takes in English and
    // Spanish came out of the verbatim pass with 2 to 6 words for 13 to 22 s
    // of voice, and the pass without it gave them back whole. The plain
    // pass's words become the transcript, with the hesitations the verbatim
    // pass heard where it heard nothing (CompleteVerbatimWithPlainPass), when
    // it decoded more words and is not flagged likely_hallucinated; the
    // answer says what was tried either way.
    std::vector<TranscriptionJob> plainJobs;
    if (verbatim) {
        for (const TranscriptionOutcome& outcome : outcomes) {
            const auto item = made.find(outcome.media_id);
            if (!outcome.ok || item == made.end() ||
                !item->second.has_envelope ||
                !item->second.transcript.likely_incomplete) {
                continue;
            }
            for (const TranscriptionJob& job : jobs) {
                if (job.media_id != outcome.media_id) continue;
                const std::filesystem::path plainStaging =
                    std::filesystem::temp_directory_path() /
                    ("cutmachine-transcript-" + job.media_id + "-plain-" +
                     GenerateUlid() + ".json");
                staged[job.media_id + "#plain"] = plainStaging;
                plainJobs.push_back({job.media_id, job.input_path,
                                     plainStaging.string(), job.source_rate});
            }
        }
    }
    if (!plainJobs.empty()) {
        WhisperSettings plainSettings = settings;
        plainSettings.verbatim = false;
        plainSettings.decoding_profile = WhisperDecodingProfile::Default;
        std::vector<TranscriptionOutcome> plainOutcomes;
        std::string batchError;
        const MediaTaskSnapshot task = RunCommandMediaTask(
            runner, MediaTaskKind::Transcription,
            "Whisper " + std::to_string(plainJobs.size()) +
                " media sans verbatim",
            [&plainJobs, &plainOutcomes, &batchError, plainSettings](
                MediaTaskContext& context, std::string& taskError) {
                const bool started =
                    GenerateAudioTranscripts(plainJobs, plainSettings, context,
                                             plainOutcomes, batchError);
                if (!started) taskError = batchError;
                return started;
            });
        const std::string taskFailure =
            task.state == MediaTaskState::Succeeded
                ? std::string()
                : MediaTaskFailure(task, "the plain transcription");
        for (TranscriptionOutcome& outcome : outcomes) {
            const auto item = made.find(outcome.media_id);
            const auto plainPath = staged.find(outcome.media_id + "#plain");
            if (item == made.end() || plainPath == staged.end()) continue;
            Made& verbatimMade = item->second;
            outcome.plain_pass_tried = true;
            outcome.verbatim_pass_words = verbatimMade.transcript.words.size();
            outcome.plain_pass_words = 0;
            const auto plainOutcome =
                std::find_if(plainOutcomes.begin(), plainOutcomes.end(),
                             [&](const TranscriptionOutcome& candidate) {
                                 return candidate.media_id == outcome.media_id;
                             });
            if (!taskFailure.empty() || plainOutcome == plainOutcomes.end() ||
                !plainOutcome->ok) {
                outcome.plain_pass_detail =
                    "the plain pass failed: " +
                    (!taskFailure.empty() ? taskFailure
                     : plainOutcome == plainOutcomes.end()
                         ? std::string("no outcome")
                         : plainOutcome->error);
                continue;
            }
            Transcript plain;
            std::string plainError;
            if (!LoadAudioTranscript(plainPath->second.string(), plain,
                                     plainError)) {
                outcome.plain_pass_detail =
                    "the plain pass cannot be read: " + plainError;
                continue;
            }
            if (TakePlainPassOverIncompleteVerbatim(
                    verbatimMade.transcript, plain, verbatimMade.envelope,
                    outcome)) {
                describeMade(outcome, verbatimMade.transcript);
            }
        }
    }

    // ITW9 -- docs/roadmap/ITW-2026-09.md. Speech that came out as "..."
    // (EllipsisOverSpeech, at least kEllipsisRepairMinSpeechCentis of it) is
    // decoded again with the ellipsis forbidden, by the pass that made the
    // words now standing: verbatim, or plain when no verbatim was asked or
    // the plain pass took the verbatim one's place. Its words take the
    // ellipses' place where they are neither a caption credit, a loop nor
    // too sparse (FillEllipsesFromPass). What a verbatim pass leaves under
    // ellipses is then asked of the plain pass, the ellipsis still
    // forbidden: on three conference takes of 15 November 2026, the
    // verbatim pass only had a caption credit to say where the plain one
    // gave 35 to 63 words. The answer says what was tried either way.
    const RationalTime ellipsisFloor{kEllipsisRepairMinSpeechCentis, 100};
    const auto redoWithoutEllipses = [&](bool verbatimRedo, bool fallback) {
        const std::string key = fallback ? "#ellipsis-plain" : "#ellipsis";
        std::vector<TranscriptionJob> ellipsisJobs;
        for (TranscriptionOutcome& outcome : outcomes) {
            const auto item = made.find(outcome.media_id);
            if (!outcome.ok || item == made.end() ||
                !item->second.has_envelope) {
                continue;
            }
            const Transcript& standing = item->second.transcript;
            const bool standsVerbatim =
                standing.verbatim && standing.plain_pass_words == 0;
            if (fallback ? !(standsVerbatim && outcome.ellipsis_pass_tried)
                         : standsVerbatim != verbatimRedo) {
                continue;
            }
            const RationalTime covered =
                EllipsisOverSpeech(standing, item->second.envelope);
            if (covered < ellipsisFloor) continue;
            if (!fallback) {
                outcome.ellipsis_pass_tried = true;
                outcome.ellipsis_speech = covered;
            }
            for (const TranscriptionJob& job : jobs) {
                if (job.media_id != outcome.media_id) continue;
                const std::filesystem::path staging =
                    std::filesystem::temp_directory_path() /
                    ("cutmachine-transcript-" + job.media_id + "-ellipsis-" +
                     GenerateUlid() + ".json");
                staged[job.media_id + key] = staging;
                ellipsisJobs.push_back({job.media_id, job.input_path,
                                        staging.string(), job.source_rate});
            }
        }
        if (ellipsisJobs.empty()) return;
        WhisperSettings ellipsisSettings = settings;
        ellipsisSettings.verbatim = verbatimRedo;
        ellipsisSettings.suppress_ellipsis = true;
        // AL4 -- a decoding profile holds for the verbatim pass it was
        // evaluated on; the plain pass decodes as the plain fallback does.
        if (!verbatimRedo) {
            ellipsisSettings.decoding_profile = WhisperDecodingProfile::Default;
        }
        std::vector<TranscriptionOutcome> ellipsisOutcomes;
        std::string batchError;
        const MediaTaskSnapshot task = RunCommandMediaTask(
            runner, MediaTaskKind::Transcription,
            "Whisper " + std::to_string(ellipsisJobs.size()) +
                (verbatimRedo ? " media sans points de suspension"
                              : " media sans points de suspension ni "
                                "verbatim"),
            [&ellipsisJobs, &ellipsisOutcomes, &batchError, ellipsisSettings](
                MediaTaskContext& context, std::string& taskError) {
                const bool started = GenerateAudioTranscripts(
                    ellipsisJobs, ellipsisSettings, context, ellipsisOutcomes,
                    batchError);
                if (!started) taskError = batchError;
                return started;
            });
        const std::string taskFailure =
            task.state == MediaTaskState::Succeeded
                ? std::string()
                : MediaTaskFailure(task, "the pass without ellipses");
        for (const TranscriptionJob& job : ellipsisJobs) {
            const auto outcome =
                std::find_if(outcomes.begin(), outcomes.end(),
                             [&](const TranscriptionOutcome& candidate) {
                                 return candidate.media_id == job.media_id;
                             });
            const auto item = made.find(job.media_id);
            if (outcome == outcomes.end() || item == made.end()) continue;
            const auto redoOutcome =
                std::find_if(ellipsisOutcomes.begin(), ellipsisOutcomes.end(),
                             [&](const TranscriptionOutcome& candidate) {
                                 return candidate.media_id == job.media_id;
                             });
            if (!taskFailure.empty() || redoOutcome == ellipsisOutcomes.end() ||
                !redoOutcome->ok) {
                outcome->ellipsis_pass_detail =
                    "the pass without ellipses failed: " +
                    (!taskFailure.empty() ? taskFailure
                     : redoOutcome == ellipsisOutcomes.end()
                         ? std::string("no outcome")
                         : redoOutcome->error);
                continue;
            }
            Transcript redo;
            std::string redoError;
            if (!LoadAudioTranscript(job.output_path, redo, redoError)) {
                outcome->ellipsis_pass_detail =
                    "the pass without ellipses cannot be read: " + redoError;
                continue;
            }
            Made& standing = item->second;
            Transcript repaired = standing.transcript;
            EllipsisRepair repair;
            const bool filled =
                FillEllipsesFromPass(repaired, redo, standing.envelope, repair);
            if (!fallback) outcome->ellipsis_spans = repair.spans;
            outcome->ellipsis_spans_refused = repair.refused;
            if (!filled) {
                if (outcome->ellipsis_spans_filled == 0) {
                    outcome->ellipsis_pass_detail =
                        repair.refused > 0
                            ? "what it decoded under the ellipses was a "
                              "caption credit, a loop or too few words"
                            : "it decoded no word under the ellipses";
                }
                continue;
            }
            std::string assessmentError;
            if (!assessMade(*outcome, repaired, standing.envelope,
                            assessmentError)) {
                outcome->ellipsis_pass_detail =
                    "the filled transcript cannot be assessed: " +
                    assessmentError;
                continue;
            }
            outcome->ellipsis_pass_detail.clear();
            outcome->ellipsis_spans_filled += repair.filled;
            outcome->ellipsis_pass_words += repair.words;
            if (fallback) outcome->ellipsis_plain_words += repair.words;
            standing.transcript = std::move(repaired);
            describeMade(*outcome, standing.transcript);
        }
    };
    redoWithoutEllipses(true, false);
    redoWithoutEllipses(false, false);
    redoWithoutEllipses(false, true);

    // PRD40 -- ROADMAP.md. The cache every agent of a series reads is not
    // replaced by a transcription that looks worse than it, unless the call
    // says so; the answer says which one the cache holds. A transcript that
    // could not be assessed is installed all the same, and said.
    for (TranscriptionOutcome& outcome : outcomes) {
        const auto item = made.find(outcome.media_id);
        if (item == made.end()) continue;
        const std::filesystem::path transcriptPath =
            base / ".cutmachine" / "transcripts" / (outcome.media_id + ".json");
        std::string saveError;
        if (!InstallTranscriptUnlessWorse(transcriptPath.string(),
                                          item->second.transcript, overwrite,
                                          outcome, saveError)) {
            outcome.ok = false;
            outcome.error =
                "cannot install the transcript in the cache: " + saveError;
        }
    }

    size_t succeeded = 0;
    std::ostringstream json;
    json << "{\"ok\":true,\"verbatim\":" << (verbatim ? "true" : "false")
         << ",\"decoding_profile\":\"" << WhisperDecodingProfileName(profile)
         << "\",\"results\":[";
    bool first = true;
    for (const TranscriptionOutcome& outcome : outcomes) {
        if (!first) json << ',';
        first = false;
        if (outcome.ok) ++succeeded;
        json << "{\"media_id\":\"" << EscapeJson(outcome.media_id)
             << "\",\"ok\":" << (outcome.ok ? "true" : "false")
             << ",\"words\":" << outcome.words << ",\"speech_assessed\":"
             << (outcome.speech_assessed ? "true" : "false");
        if (outcome.speech_assessed) {
            json << ",\"measured_speech_duration\":{\"value\":"
                 << outcome.measured_speech_duration.value
                 << ",\"rate\":" << outcome.measured_speech_duration.rate << "}"
                 << ",\"likely_hallucinated\":"
                 << (outcome.likely_hallucinated ? "true" : "false")
                 << ",\"likely_incomplete\":"
                 << (outcome.likely_incomplete ? "true" : "false")
                 << ",\"known_hallucination_phrase\":"
                 << (outcome.known_hallucination_phrase ? "true" : "false");
        }
        if (!outcome.ok)
            json << ",\"error\":\"" << EscapeJson(outcome.error) << "\"";
        else
            json << ",\"path\":\""
                 << EscapeJson((base / ".cutmachine" / "transcripts" /
                                (outcome.media_id + ".json"))
                                   .string())
                 << "\"";
        if (outcome.ok && !outcome.text.empty()) {
            json << ",\"text\":\"" << EscapeJson(outcome.text) << "\"";
            if (outcome.text_truncated) json << ",\"text_truncated\":true";
        }
        // ITW9 -- the language whisper heard, which align_words aligns in.
        if (outcome.ok && !outcome.detected_language.empty()) {
            json << ",\"detected_language\":\""
                 << EscapeJson(outcome.detected_language) << "\"";
        }
        // ITW9 -- the verbatim pass came out likely_incomplete and was done
        // again without the verbatim prompt: what each pass decoded, and
        // whether the plain one now stands, with the hesitations kept.
        if (outcome.plain_pass_tried) {
            json << ",\"plain_pass\":{\"used\":"
                 << (outcome.plain_pass_used ? "true" : "false")
                 << ",\"verbatim_words\":" << outcome.verbatim_pass_words
                 << ",\"plain_words\":" << outcome.plain_pass_words;
            if (outcome.plain_pass_used) {
                json << ",\"kept_verbatim_words\":"
                     << outcome.kept_verbatim_words;
            } else {
                json << ",\"detail\":\""
                     << EscapeJson(outcome.plain_pass_detail) << "\"";
            }
            json << "}";
        }
        // ITW9 -- speech that came out as "..." was decoded again with the
        // ellipsis forbidden: how much, and what took the ellipses' place.
        if (outcome.ellipsis_pass_tried) {
            json << ",\"ellipsis_pass\":{\"speech_under_ellipses\":{\"value\":"
                 << outcome.ellipsis_speech.value
                 << ",\"rate\":" << outcome.ellipsis_speech.rate << "}"
                 << ",\"ellipses\":" << outcome.ellipsis_spans
                 << ",\"filled\":" << outcome.ellipsis_spans_filled
                 << ",\"refused\":" << outcome.ellipsis_spans_refused
                 << ",\"words\":" << outcome.ellipsis_pass_words;
            if (outcome.ellipsis_plain_words > 0) {
                json << ",\"plain_words\":" << outcome.ellipsis_plain_words;
            }
            if (!outcome.ellipsis_pass_detail.empty()) {
                json << ",\"detail\":\""
                     << EscapeJson(outcome.ellipsis_pass_detail) << "\"";
            }
            json << "}";
        }
        // PRD40 -- what was made is above; what the cache holds, here.
        if (outcome.cache_kept) {
            json << ",\"cache_kept\":true,\"cached_words\":"
                 << outcome.cached_words << ",\"cache_detail\":\""
                 << EscapeJson(outcome.cache_kept_reason +
                               "; the cached transcript is kept and this "
                               "one discarded -- send overwrite: true to "
                               "install it anyway")
                 << "\"";
        }
        json << "}";
    }
    json << "],\"skipped\":[";
    for (size_t index = 0; index < skipped.size(); ++index) {
        if (index) json << ',';
        json << "{\"media_id\":\"" << EscapeJson(skipped[index].media_id)
             << "\",\"reason\":\"" << EscapeJson(skipped[index].reason)
             << "\"}";
    }
    json << "],\"transcribed\":" << succeeded
         << ",\"skipped_count\":" << skipped.size() << "}\n";
    output = json.str();
    // A batch in which every named media failed is a failed command: a caller
    // that only checks the exit status must not read it as done.
    if (!outcomes.empty() && succeeded == 0) {
        const std::string detail = outcomes.size() == 1
                                       ? outcomes.front().error
                                       : "all transcription jobs failed";
        return FailCliCommand("IoError", detail, output);
    }
    return 0;
}

int AnalyzeShotQualityCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output,
                              const MediaTaskRunner& runner,
                              const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    const auto source = std::find_if(
        project.sources.begin(), project.sources.end(),
        [&](const DocumentSource& item) { return item.id == mediaId; });
    if (media == project.rushes.end() || source == project.sources.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (!media->has_video) {
        output = ErrorJson(EditError::InvalidOperation,
                           "media_id '" + mediaId +
                               "' is audio-only; shot quality requires "
                               "a video stream");
        return 1;
    }

    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path reportPath =
        base / ".cutmachine" / "shotquality" / (mediaId + ".json");
    // IA5 -- through the runner, as the transcriptions: a background MCP
    // task has to be able to stop this decode from outside.
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::ShotQuality, "Analyse image " + media->filename,
        [input, reportPath, mediaId, duration = source->duration](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateShotQuality(input.string(), reportPath.string(),
                                       mediaId, duration, ShotQualitySettings{},
                                       context, taskError);
        });
    if (task.state != MediaTaskState::Succeeded) {
        output = ErrorJson(EditError::IoError,
                           MediaTaskFailure(task, "shot quality analysis"));
        return 1;
    }
    ShotQualityReport report;
    std::string loadError;
    const bool loaded = LoadShotQuality(reportPath.string(), report, loadError);
    output =
        "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
        "\",\"path\":\"" + EscapeJson(reportPath.string()) +
        "\",\"samples\":" + std::to_string(loaded ? report.samples.size() : 0) +
        ",\"median_sharpness\":" +
        std::to_string(loaded ? report.median_sharpness : 0) + "}\n";
    return 0;
}

// F1.6 -- source identity, media path and cache ownership are resolved in
// the same headless command regardless of which agent surface requested it.
int AnalyzeAudioBeatsCommand(const std::string& projectPath,
                             const std::string& mediaId, std::string& output,
                             const MediaTaskRunner& runner,
                             const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    const auto source = std::find_if(
        project.sources.begin(), project.sources.end(),
        [&](const DocumentSource& item) { return item.id == mediaId; });
    if (media == project.rushes.end() || source == project.sources.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (media->metadata_complete && !media->has_audio) {
        output = ErrorJson(
            EditError::InvalidOperation,
            "media_id '" + mediaId + "' has no audio stream for beat analysis");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) {
        input = base / input;
    }
    input = input.lexically_normal();
    const std::filesystem::path cachePath =
        base / ".cutmachine" / "beats" / (mediaId + ".beats");
    // IA5 -- through the runner, as the transcriptions.
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::Beat, "Analyse musicale " + media->filename,
        [input, cachePath, duration = source->duration](
            MediaTaskContext& context, std::string& taskError) {
            return DetectAudioBeats(input.string(), cachePath.string(),
                                    duration, BeatDetectionSettings{}, context,
                                    taskError);
        });
    if (task.state != MediaTaskState::Succeeded) {
        output = ErrorJson(EditError::IoError,
                           MediaTaskFailure(task, "audio beat analysis"));
        return 1;
    }
    BeatDetectionResult result;
    if (!LoadAudioBeats(cachePath.string(), result, error)) {
        output =
            ErrorJson(EditError::IoError,
                      "unable to read produced audio beat cache: " + error);
        return 1;
    }
    output = "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
             "\",\"path\":\"" + EscapeJson(cachePath.string()) +
             "\",\"onsets\":" + std::to_string(result.onsets.size()) + "}\n";
    return 0;
}

// PA3 -- docs/roadmap/PARITE-2026-11.md.
int AnalyzeSpeakersCommand(const std::string& projectPath,
                           const std::string& mediaId, int32_t speakers,
                           std::string& output, const MediaTaskRunner& runner,
                           const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    if (media == project.rushes.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (media->metadata_complete && !media->has_audio) {
        output = ErrorJson(
            EditError::InvalidOperation,
            "media_id '" + mediaId + "' has no sound to tell voices apart in");
        return 1;
    }
    if (speakers < 1 || speakers > kMaxDiarizedSpeakers) {
        output = ErrorJson(EditError::ValidationFailed,
                           "'speakers' must lie in [1, " +
                               std::to_string(kMaxDiarizedSpeakers) +
                               "]: how many voices the rush holds");
        return 1;
    }
    const DiarizationSettings settings = DiarizationSettings::FromEnvironment();
    if (const std::string missing = MissingDiarizationInstallation(settings);
        !missing.empty()) {
        output = ErrorJson(EditError::IoError, missing);
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path cachePath =
        base / ".cutmachine" / "speakers" / (mediaId + ".json");

    // The stretches of speech: the rush's words when Whisper has read it,
    // joined across the gaps under three tenths of a second.
    std::string speech;
    {
        const std::filesystem::path transcriptPath =
            base / ".cutmachine" / "transcripts" / (mediaId + ".json");
        Transcript transcript;
        std::string transcriptError;
        std::error_code exists;
        if (std::filesystem::exists(transcriptPath, exists) &&
            LoadAudioTranscript(transcriptPath.string(), transcript,
                                transcriptError)) {
            const std::vector<SpeechStretch> stretches =
                SpeechStretchesOfWords(transcript.words, {3, 10});
            if (!stretches.empty()) speech = SpeechStretchesJson(stretches);
        }
    }

    const std::filesystem::path work =
        std::filesystem::temp_directory_path() /
        ("cutmachine-speakers-" + GenerateUlid());
    std::error_code directoryError;
    std::filesystem::create_directories(work, directoryError);
    if (directoryError) {
        output =
            ErrorJson(EditError::IoError, "unable to create '" + work.string() +
                                              "': " + directoryError.message());
        return 1;
    }
    const auto cleanup = [&work] {
        std::error_code ignored;
        std::filesystem::remove_all(work, ignored);
    };
    DiarizationRequest request;
    request.audio_path = (work / "sound.wav").string();
    request.report_path = (work / "report.json").string();
    request.speakers = speakers;
    if (!speech.empty()) {
        request.speech_path = (work / "speech.json").string();
        std::ofstream file(request.speech_path,
                           std::ios::binary | std::ios::trunc);
        file << speech;
        file.close();
        if (!file) {
            cleanup();
            output = ErrorJson(EditError::IoError,
                               "unable to write '" + request.speech_path + "'");
            return 1;
        }
    }
    // IA5 -- through the runner, as the transcriptions: a background MCP
    // task has to be able to stop the decode and the model from outside.
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::SpeakerDiarization,
        "Locuteurs " + media->filename,
        [input, request, settings](MediaTaskContext& context,
                                   std::string& taskError) {
            // What the model was trained on: mono, 16 bits, 16 kHz.
            process::Request decode;
            decode.arguments = {
                "",      "-hide_banner", "-loglevel",
                "error", "-nostdin",     "-y",
                "-i",    input.string(), "-map",
                "0:a:0", "-vn",          "-ac",
                "1",     "-ar",          std::to_string(kDiarizationSampleRate),
                "-c:a",  "pcm_s16le",    request.audio_path};
            decode.discard_input = true;
            decode.standard_output = process::Stream::kDiscard;
            decode.standard_error = process::Stream::kPipe;
            decode.error_tail_bytes = 4096;
            decode.cancelled = [&context] { return context.Cancelled(); };
            context.SetProgress(0.0, "Son du rush");
            const process::Result decoded = ffmpeg::Run(decode);
            if (!decoded.Started()) {
                taskError = ffmpeg::StartError(decoded, "the decode's pipes");
                return false;
            }
            if (decoded.cancelled || context.Cancelled()) {
                taskError = "diarization cancelled";
                return false;
            }
            if (!decoded.Succeeded()) {
                taskError = decoded.error_tail.empty()
                                ? "FFmpeg could not read the rush's sound"
                                : decoded.error_tail;
                return false;
            }
            return RunDiarization(settings, request, context, taskError);
        });
    if (task.state != MediaTaskState::Succeeded) {
        cleanup();
        output = ErrorJson(EditError::IoError,
                           MediaTaskFailure(task, "speaker diarization"));
        return 1;
    }
    std::ifstream reportFile(request.report_path, std::ios::binary);
    const std::string report((std::istreambuf_iterator<char>(reportFile)),
                             std::istreambuf_iterator<char>());
    cleanup();
    SpeakerDiarization diarization;
    if (!ReadDiarizationReport(report, mediaId, diarization, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    // Written beside, then put in place: a reader never meets half a cache.
    std::error_code cacheError;
    std::filesystem::create_directories(cachePath.parent_path(), cacheError);
    const std::filesystem::path partial =
        cachePath.parent_path() / (mediaId + ".json.partial");
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << SerializeSpeakerDiarization(diarization);
        file.close();
        if (!file) cacheError = std::make_error_code(std::errc::io_error);
    }
    if (!cacheError) std::filesystem::rename(partial, cachePath, cacheError);
    if (cacheError) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        output =
            ErrorJson(EditError::IoError,
                      "unable to write the speakers cache '" +
                          cachePath.string() + "': " + cacheError.message());
        return 1;
    }
    output =
        "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
        "\",\"path\":\"" + EscapeJson(cachePath.string()) +
        "\",\"speakers\":" + std::to_string(diarization.speakers) +
        ",\"speakers_heard\":" + std::to_string(diarization.speakers_heard) +
        ",\"segments\":" + std::to_string(diarization.segments.size()) +
        ",\"speech_from\":\"" + EscapeJson(diarization.speech_from) + "\"}\n";
    return 0;
}

int ShotQualityReportCommand(const std::string& projectPath,
                             std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    // BENCH-2026-09 (B3) -- as list_shot_quality: an unreadable cache is
    // named under unreadable_caches, not read as a source never analysed,
    // and does not hide the other sources' grades.
    std::map<Ulid, ShotQualityReport> reports;
    std::vector<Ulid> seen;
    std::vector<UnreadableShotQualityCache> unreadable;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (std::find(seen.begin(), seen.end(), clip.source_id) !=
                seen.end()) {
                continue;
            }
            seen.push_back(clip.source_id);
            ShotQualityReport report;
            const std::filesystem::path path = base / ".cutmachine" /
                                               "shotquality" /
                                               (clip.source_id + ".json");
            const ShotQualityCacheRead read =
                ReadShotQualityCache(path.string(), report);
            if (read.state == ShotQualityCacheState::Loaded) {
                reports.emplace(clip.source_id, std::move(report));
            } else if (read.state == ShotQualityCacheState::Unreadable) {
                unreadable.push_back({clip.source_id, read});
            }
        }
    }
    // PRD39 -- the report list_shot_quality answers, with the command
    // line's "ok" in front.
    output = WithCliOk(
        DescribeShotQualityForAgent(document, reports, ShotQualityThresholds{},
                                    ShotSegmentationSettings{}, unreadable) +
        "\n");
    return 0;
}

// R8 -- docs/roadmap/REEL-2026-09.md. The isolated-frame pass over what the
// active timeline actually reads. One command for both surfaces of the
// ticket: with `analyze` it decodes the ranges no cache covers and writes
// them, without it it only reports what the caches already hold. The split
// is list_shot_quality's -- a report must never spend minutes nobody asked
// for -- and the pass is per timeline rather than per media because the
// ranges are a property of the edit, not of the rush.
int IsolatedFramesCommand(const std::string& projectPath,
                          const std::string& timelineId, bool analyze,
                          std::string& output, const MediaTaskRunner& runner,
                          const std::vector<Ulid>& mediaIds,
                          const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    if (!timelineId.empty() && project.FindTimeline(timelineId) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + timelineId + "'");
        return 1;
    }
    const Document document = timelineId.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(timelineId);
    std::vector<IsolatedFrameScanTarget> targets;
    if (!mediaIds.empty()) {
        if (!PlanMediaIsolatedFrameScan(document, mediaIds, targets, error)) {
            output = ErrorJson(error.rfind("unknown media_id", 0) == 0
                                   ? EditError::UnknownMedia
                                   : EditError::ValidationFailed,
                               error);
            return 1;
        }
    } else if (!PlanIsolatedFrameScan(document, IsolatedFrameScanSettings{},
                                      targets, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::map<Ulid, IsolatedFrameScan> scans;
    std::map<Ulid, IsolatedFrameCacheRead> reads;
    for (const IsolatedFrameScanTarget& target : targets) {
        const std::filesystem::path cachePath = base / ".cutmachine" /
                                                "isolatedframes" /
                                                (target.media_id + ".json");
        IsolatedFrameScan scan;
        IsolatedFrameCacheRead read =
            ReadIsolatedFrameCache(cachePath.string(), scan);
        const bool covered = read.state == IsolatedFrameCacheState::Loaded &&
                             UncoveredSpans(target, scan).empty();
        if (analyze && !covered) {
            const auto media =
                std::find_if(project.rushes.begin(), project.rushes.end(),
                             [&](const LibraryMedia& item) {
                                 return item.id == target.media_id;
                             });
            if (media == project.rushes.end() || !media->has_video) {
                read.state = IsolatedFrameCacheState::Unreadable;
                read.detail = "media '" + target.media_id +
                              "' has no video stream to scan";
                reads[target.media_id] = read;
                continue;
            }
            std::filesystem::path input(media->path);
            if (input.is_relative()) input = base / input;
            input = input.lexically_normal();
            const MediaTaskSnapshot task = RunCommandMediaTask(
                runner, MediaTaskKind::IsolatedFrames,
                "Images isolées " + media->filename,
                [input, cachePath, target](MediaTaskContext& context,
                                           std::string& taskError) {
                    return GenerateIsolatedFrameScan(
                        input.string(), cachePath.string(), target.media_id,
                        target.rate, target.spans,
                        IsolatedFrameMeasureSettings{}, context, taskError);
                });
            if (task.state != MediaTaskState::Succeeded) {
                output =
                    ErrorJson(EditError::IoError,
                              MediaTaskFailure(task, "isolated frame scan"));
                return 1;
            }
            read = ReadIsolatedFrameCache(cachePath.string(), scan);
        }
        reads[target.media_id] = read;
        if (read.state == IsolatedFrameCacheState::Loaded)
            scans.emplace(target.media_id, std::move(scan));
    }
    output = DescribeIsolatedFramesForAgent(document, targets, scans, reads,
                                            IsolatedFrameSettings{}) +
             "\n";
    return 0;
}

int TimelineStatsCommand(const std::string& projectPath, std::string& output,
                         const std::string& timelineId) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    if (!timelineId.empty() && project.FindTimeline(timelineId) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + timelineId + "'");
        return 1;
    }
    TimelineStats stats;
    if (!CalculateTimelineStats(timelineId.empty()
                                    ? project.MakeActiveDocument()
                                    : project.MakeDocument(timelineId),
                                stats, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    output = "{\"ok\":true,\"stats\":" + SerializeTimelineStats(stats) + "}\n";
    return 0;
}

int CheckTimelineCommand(const std::string& projectPath,
                         const std::string& timelineId,
                         const std::string& policyJson,
                         const std::string& filePath, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    if (!timelineId.empty() && project.FindTimeline(timelineId) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + timelineId + "'");
        return 1;
    }
    TimelinePolicy policy;
    if (!policyJson.empty()) {
        mcp_json::Value value;
        if (!mcp_json::Value::Parse(policyJson, value, error)) {
            output = ErrorJson(EditError::ValidationFailed,
                               "--policy is not JSON: " + error);
            return 1;
        }
        if (!ParseTimelinePolicy(value, policy, error)) {
            output = ErrorJson(EditError::ValidationFailed, error);
            return 1;
        }
        // DEC6 -- the charter the policy names, from this user's library.
        if (!policy.kit_name.empty()) {
            MotionKitInfo kit;
            if (!FindMotionLibraryKit(CurrentMotionLibraryLocation(),
                                      policy.kit_name, kit, error)) {
                output = ErrorJson(EditError::ValidationFailed, error);
                return 1;
            }
            policy.kit = std::move(kit);
        }
    }
    const Document document = timelineId.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(timelineId);
    TimelineFileMeasures measures;
    if (!filePath.empty()) {
        int64_t window = 0;
        if (!FinalWindowFrames(document.sequence, policy, window, error)) {
            output = ErrorJson(EditError::ValidationFailed, error);
            return 1;
        }
        // PRD1 -- the binary FfmpegBinary.h resolves.
        if (!MeasureDeliveredFile(document, policy, filePath, std::string(),
                                  measures, error)) {
            output = ErrorJson(EditError::IoError, error);
            return 1;
        }
    }
    TimelineCheckReport report;
    if (!CheckTimeline(document, policy, filePath.empty() ? nullptr : &measures,
                       report, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    // As --check-setup: "ok" is the verdict, and so is the exit status.
    const std::string body = SerializeTimelineCheck(report);
    output = std::string("{\"ok\":") + (report.Passed() ? "true" : "false") +
             "," + body.substr(1) + "\n";
    return report.Passed() ? 0 : 1;
}

namespace {

int TimelineSheetCommand(const std::string& projectPath,
                         const std::string& outputPath,
                         const std::string& timelineId, int32_t maximumImages,
                         TimelineSheetKind kind, bool graded,
                         std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(selected) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selected + "'");
        return 1;
    }
    TimelineSheetSettings settings;
    settings.maximum_images = maximumImages;
    settings.graded = graded;
    const Document document = project.MakeDocument(selected);
    TimelineSheetPlan plan;
    if (!BuildTimelineSheetPlan(document, kind, settings, plan, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    // PRD39 -- ROADMAP.md. Nothing to show is said before the render, and
    // not as an IoError (EmptyTimelineSheetReason).
    if (plan.frames.empty()) {
        output = ErrorJson(EditError::ValidationFailed,
                           EmptyTimelineSheetReason(plan));
        return 1;
    }
    const std::filesystem::path projectFile =
        std::filesystem::absolute(projectPath);
    std::string jpeg;
    if (!RenderTimelineSheet(document, projectFile.parent_path(), plan,
                             settings, jpeg, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    if (!WriteFile(outputPath, jpeg, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    output = "{\"ok\":true,\"output_path\":\"" + EscapeJson(outputPath) +
             "\",\"sheet\":" + SerializeTimelineSheetPlan(plan, graded) + "}\n";
    return 0;
}

}  // namespace

int ContactSheetCommand(const std::string& projectPath,
                        const std::string& outputPath,
                        const std::string& timelineId, int32_t maximumImages,
                        std::string& output, bool graded) {
    return TimelineSheetCommand(projectPath, outputPath, timelineId,
                                maximumImages, TimelineSheetKind::Contact,
                                graded, output);
}

int MediaSheetCommand(const std::string& projectPath,
                      const std::string& outputPath,
                      const std::vector<Ulid>& mediaIds, int32_t maximumImages,
                      std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    if (maximumImages <= 0 || maximumImages > 64) {
        output = ErrorJson(EditError::ValidationFailed,
                           "max-images must be between 1 and 64");
        return 1;
    }
    TimelineSheetSettings settings;
    settings.maximum_images = maximumImages;
    const Document document = project.MakeActiveDocument();
    TimelineSheetPlan plan;
    if (!BuildMediaSheetPlan(document, mediaIds, settings, plan, error)) {
        output = ErrorJson(error.rfind("unknown media_id", 0) == 0
                               ? EditError::UnknownMedia
                               : EditError::ValidationFailed,
                           error);
        return 1;
    }
    std::string jpeg;
    if (!RenderTimelineSheet(
            document, std::filesystem::absolute(projectPath).parent_path(),
            plan, settings, jpeg, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    if (!WriteFile(outputPath, jpeg, error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    output = "{\"ok\":true,\"output_path\":\"" + EscapeJson(outputPath) +
             "\",\"sheet\":" + SerializeTimelineSheetPlan(plan) + "}\n";
    return 0;
}

int CutSheetCommand(const std::string& projectPath,
                    const std::string& outputPath,
                    const std::string& timelineId, int32_t maximumImages,
                    std::string& output, bool graded) {
    return TimelineSheetCommand(projectPath, outputPath, timelineId,
                                maximumImages, TimelineSheetKind::Cuts, graded,
                                output);
}

// RC1 -- docs/roadmap/RACCORDS-2026-10.md. review_cuts from the command
// line: the same review (CutReview.h), each window rendered here.
int ReviewCutsCommand(const std::string& projectPath,
                      const std::string& outputPath,
                      const std::string& timelineId,
                      const std::vector<int64_t>& frames, int32_t maximumBands,
                      std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(selected) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selected + "'");
        return 1;
    }
    const Document document = project.MakeDocument(selected);
    const std::filesystem::path projectFile =
        std::filesystem::absolute(projectPath);
    CutReviewRequest request;
    request.frames = frames;
    request.max_bands = maximumBands;
    bool renderFailed = false;
    const CutInspectionRenderer render =
        [&](const CutInspectionPlan& plan,
            const CutInspectionRenderSettings& settings,
            CutInspectionResult& result, std::string& renderError) {
            const bool rendered = RenderCutInspection(
                document, projectFile, plan, settings, result, renderError);
            renderFailed = !rendered;
            return rendered;
        };
    CutReviewReport report;
    std::vector<std::string> bands;
    if (!ReviewCuts(document, request, render, report, bands, error)) {
        output = ErrorJson(
            renderFailed ? EditError::IoError : EditError::ValidationFailed,
            error);
        return 1;
    }
    std::string written;
    if (!bands.empty()) {
        std::string jpeg;
        if (!StackInspectionPictures(bands, std::string(), jpeg, error) ||
            !WriteFile(outputPath, jpeg, error)) {
            output = ErrorJson(EditError::IoError, error);
            return 1;
        }
        written = outputPath;
    }
    output = "{\"ok\":true,\"output_path\":" +
             (written.empty() ? std::string("null")
                              : "\"" + EscapeJson(written) + "\"") +
             ",\"review\":" + SerializeCutReview(report) + "}\n";
    return 0;
}

namespace {

// "num/den" or an integer, exactly; false for anything else.
bool ParseCliFraction(const std::string& text, EffectParamValue& value) {
    const size_t slash = text.find('/');
    const std::string numText = text.substr(0, slash);
    const std::string denText =
        slash == std::string::npos ? "1" : text.substr(slash + 1);
    const auto parse = [](const std::string& part, int32_t& out) {
        if (part.empty()) return false;
        size_t used = 0;
        long long parsed = 0;
        try {
            parsed = std::stoll(part, &used);
        } catch (const std::exception&) {
            return false;
        }
        if (used != part.size() ||
            parsed < std::numeric_limits<int32_t>::min() ||
            parsed > std::numeric_limits<int32_t>::max())
            return false;
        out = static_cast<int32_t>(parsed);
        return true;
    };
    return parse(numText, value.num) && parse(denText, value.den) &&
           value.den != 0;
}

// "x,y,width,height", each an exact num/den; an empty text leaves the
// region absent.
bool ParseCliRegion(const std::string& text, ColorMeasureRegion& region) {
    if (text.empty()) return true;
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) parts.push_back(part);
    region.present = true;
    EffectParamValue* values[] = {&region.x, &region.y, &region.width,
                                  &region.height};
    bool parsed = parts.size() == 4;
    for (size_t index = 0; parsed && index < 4; ++index)
        parsed = ParseCliFraction(parts[index], *values[index]);
    return parsed;
}

// The project and the timeline a colour command reads.
bool LoadColorDocument(const std::string& projectPath,
                       const std::string& timelineId, Document& document,
                       std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return false;
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(selected) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selected + "'");
        return false;
    }
    document = project.MakeDocument(selected);
    return true;
}

}  // namespace

int MeasureColorCommand(const std::string& projectPath,
                        const std::string& timelineId,
                        const std::vector<Ulid>& clipIds,
                        const std::string& position, const std::string& region,
                        std::string& output) {
    std::string error;
    Document document;
    if (!LoadColorDocument(projectPath, timelineId, document, output)) return 1;
    ColorMeasurePlan plan;
    plan.clip_ids = clipIds;
    plan.position = position;
    if (!ParseCliRegion(region, plan.region)) {
        output = ErrorJson(EditError::ValidationFailed,
                           "--region takes x,y,width,height, each an exact "
                           "num/den");
        return 1;
    }
    for (const Ulid& clipId : plan.clip_ids) {
        if (document.FindClip(clipId) == nullptr) {
            output = ErrorJson(EditError::UnknownClip,
                               "unknown clip_id '" + clipId + "'");
            return 1;
        }
    }
    std::vector<ColorClipMeasurement> results;
    if (!RenderColorMeasurement(
            document,
            std::filesystem::absolute(projectPath).parent_path().string(), plan,
            ColorMeasureSettings{}, results, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    output = "{\"ok\":true,\"measure\":" +
             ColorMeasurementsValue(document, plan, results).Dump() + "}\n";
    return 0;
}

int ExportCdlCommand(const std::string& projectPath,
                     const std::string& timelineId,
                     const std::vector<Ulid>& clipIds, const std::string& file,
                     int resolveNode, std::string& output) {
    Document document;
    if (!LoadColorDocument(projectPath, timelineId, document, output)) return 1;
    std::vector<ColorCdl> cdls;
    ColorGradeSpace space = ColorGradeSpace::Display;
    std::string error;
    if (!ClipColorCdls(document, clipIds, cdls, space, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    if (!file.empty() &&
        !WriteColorCdlFile(file, cdls, ColorCdlSpaceName(space), error)) {
        output = ErrorJson(EditError::IoError, error);
        return 1;
    }
    output = "{\"ok\":true,\"cdl\":" +
             ExportedCdlsValue(cdls, space, resolveNode, file).Dump() + "}\n";
    return 0;
}

int ApplyCdlCommand(const std::string& projectPath,
                    const std::string& timelineId,
                    const std::vector<Ulid>& clipIds, const std::string& space,
                    const std::string& file, const std::string& id,
                    const ResolveCdlMap& resolve, bool preview,
                    std::string& output) {
    ColorGradeSpace declared;
    if (!ParseColorCdlSpace(space, declared)) {
        output = ErrorJson(EditError::ValidationFailed,
                           "--space is the values the CDL was made on: "
                           "acescct, linear_rec709, linear_rec2020 or display");
        return 1;
    }
    std::string error;
    ColorCdl cdl;
    if (!file.empty() && (!resolve.slope.empty() || !resolve.offset.empty() ||
                          !resolve.power.empty())) {
        output = ErrorJson(EditError::ValidationFailed,
                           "a CDL comes from --file or from --slope, "
                           "--offset and --power, not both");
        return 1;
    }
    if (!file.empty()) {
        std::vector<ColorCdl> cdls;
        std::string inputDescription;
        if (!ReadColorCdlFile(file, cdls, inputDescription, error)) {
            output = ErrorJson(EditError::IoError, error);
            return 1;
        }
        const auto named =
            std::find_if(cdls.begin(), cdls.end(),
                         [&](const ColorCdl& item) { return item.id == id; });
        if ((!id.empty() && named == cdls.end()) ||
            (id.empty() && cdls.size() > 1)) {
            output = ErrorJson(
                EditError::ValidationFailed,
                id.empty() ? "the file holds several "
                             "corrections; name one with --id"
                           : "the file holds no correction '" + id + "'");
            return 1;
        }
        cdl = id.empty() ? cdls.front() : *named;
    } else if (!ParseResolveCdlMap(resolve, cdl, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    Document document;
    if (!LoadColorDocument(projectPath, timelineId, document, output)) return 1;
    std::vector<Operation> operations;
    for (const Ulid& clipId : clipIds) {
        ClipCdlPlacement placement;
        if (!PlaceClipCdl(document, clipId, cdl, declared, placement, error)) {
            output = ErrorJson(EditError::ValidationFailed, error);
            return 1;
        }
        operations.push_back(
            SetClipEffectsOperation{clipId, std::move(placement.effects)});
    }
    if (operations.empty()) {
        output = ErrorJson(EditError::ValidationFailed, "no clip named");
        return 1;
    }
    const Operation operation =
        operations.size() == 1
            ? operations.front()
            : Operation{BatchOperation{"CDL", std::move(operations)}};
    const std::string json = SerializeOperation(operation);
    if (preview) {
        output = "{\"ok\":true,\"operation\":" + json + "}\n";
        return 0;
    }
    return ApplyOperationCommand(projectPath, json, output, timelineId);
}

int MatchColorCommand(const std::string& projectPath,
                      const std::string& timelineId, const Ulid& clipId,
                      const Ulid& referenceClipId, const std::string& position,
                      const std::string& referencePosition,
                      const std::string& region,
                      const std::string& referenceRegion,
                      const std::string& match, std::string& output) {
    std::string error;
    Document document;
    if (!LoadColorDocument(projectPath, timelineId, document, output)) return 1;
    ColorMatchPlan plan;
    plan.clip_id = clipId;
    plan.reference_clip_id = referenceClipId;
    plan.position = position;
    plan.reference_position = referencePosition;
    if (!ParseCliRegion(region, plan.region) ||
        !ParseCliRegion(referenceRegion, plan.reference_region)) {
        output = ErrorJson(EditError::ValidationFailed,
                           "--region and --reference-region take "
                           "x,y,width,height, each an exact num/den");
        return 1;
    }
    if (!match.empty()) {
        plan.aspects = {false, false, false};
        std::stringstream stream(match);
        std::string aspect;
        while (std::getline(stream, aspect, ',')) {
            if (aspect == "exposure") {
                plan.aspects.exposure = true;
            } else if (aspect == "balance") {
                plan.aspects.balance = true;
            } else if (aspect == "saturation") {
                plan.aspects.saturation = true;
            } else {
                output = ErrorJson(EditError::ValidationFailed,
                                   "--match takes exposure, balance and "
                                   "saturation");
                return 1;
            }
        }
    }
    for (const Ulid* id : {&plan.clip_id, &plan.reference_clip_id}) {
        if (document.FindClip(*id) == nullptr) {
            output = ErrorJson(EditError::UnknownClip,
                               "unknown clip_id '" + *id + "'");
            return 1;
        }
    }
    ColorMatchResult result;
    if (!RenderColorMatch(
            document,
            std::filesystem::absolute(projectPath).parent_path().string(), plan,
            ColorMeasureSettings{}, result, error)) {
        output = ErrorJson(EditError::ValidationFailed, error);
        return 1;
    }
    output = "{\"ok\":true,\"match\":" +
             ColorMatchValue(document, plan, result).Dump() + "}\n";
    return 0;
}

int AlignTranscriptsCommand(const std::string& projectPath, bool apply,
                            std::string& output, const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::ostringstream out;
    out << "{\"ok\":true,\"applied\":" << (apply ? "true" : "false")
        << ",\"sources\":[";
    bool first = true;
    for (const LibraryMedia& media : project.rushes) {
        Transcript transcript;
        SpeechOnsetReport envelope;
        std::string message;
        const std::filesystem::path transcriptPath =
            base / ".cutmachine" / "transcripts" / (media.id + ".json");
        const std::filesystem::path envelopePath =
            base / ".cutmachine" / "speechonset" / (media.id + ".json");
        if (!LoadAudioTranscript(transcriptPath.string(), transcript, message))
            continue;
        if (!LoadSpeechOnset(envelopePath.string(), envelope, message))
            continue;
        Transcript aligned;
        TranscriptAlignmentReport report;
        if (!AlignTranscriptToSpeech(transcript, envelope,
                                     TranscriptAlignmentSettings{}, aligned,
                                     report, message))
            continue;
        // QC-2026-09 (A1) -- the write is what removes the probing loop. A
        // report alone leaves every downstream reader (remove_words,
        // clean_disfluencies, the interview short) still cutting on the
        // boundaries the signal contradicts, so the correction has to reach
        // the cache the rest of the toolchain actually reads. It stays
        // opt-in, and a failed write is reported per source rather than
        // aborting the pass: the remaining sources are still worth aligning.
        std::string writeError;
        bool written = false;
        if (apply) {
            aligned.speech_aligned = true;
            written = SaveAudioTranscript(transcriptPath.string(), aligned,
                                          writeError);
        }
        if (!first) out << ',';
        first = false;
        out << "{\"media_id\":\"" << EscapeJson(media.id)
            << "\",\"filename\":\"" << EscapeJson(media.filename)
            << "\",\"written\":" << (written ? "true" : "false");
        if (apply && !written)
            out << ",\"write_error\":\"" << EscapeJson(writeError) << "\"";
        out << ",\"report\":" << SerializeTranscriptAlignmentReport(report)
            << "}";
    }
    out << "]}\n";
    output = out.str();
    return 0;
}

int AlignWordsCommand(const std::string& projectPath,
                      const std::vector<std::string>& mediaIds,
                      const std::string& textsJson, std::string& output,
                      const MediaTaskRunner& runner, const Project* stored) {
    using mcp_json::Value;
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    Value texts = Value::MakeObject();
    if (!textsJson.empty() &&
        (!Value::Parse(textsJson, texts, error) || !texts.IsObject())) {
        output = ErrorJson(EditError::ValidationFailed,
                           "texts must be a JSON object {media_id: text}");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    const std::filesystem::path cache = base / ".cutmachine";
    const auto hasTranscript = [&](const Ulid& id) {
        std::error_code ignored;
        return std::filesystem::exists(cache / "transcripts" / (id + ".json"),
                                       ignored);
    };
    // The rushes named, or every rush Whisper has read.
    std::vector<const LibraryMedia*> wanted;
    if (mediaIds.empty()) {
        for (const LibraryMedia& media : project.rushes)
            if (media.has_audio && hasTranscript(media.id))
                wanted.push_back(&media);
    } else {
        for (const std::string& id : mediaIds) {
            const auto found = std::find_if(
                project.rushes.begin(), project.rushes.end(),
                [&](const LibraryMedia& media) { return media.id == id; });
            if (found == project.rushes.end()) {
                output = ErrorJson(EditError::UnknownMedia,
                                   "unknown media_id '" + id + "'");
                return 1;
            }
            wanted.push_back(&*found);
        }
    }
    for (const auto& [id, text] : texts.AsObject()) {
        if (!text.IsString() || std::none_of(wanted.begin(), wanted.end(),
                                             [&](const LibraryMedia* media) {
                                                 return media->id == id;
                                             })) {
            output = ErrorJson(EditError::ValidationFailed,
                               "texts names '" + id +
                                   "', which is not a rush being aligned, or "
                                   "gives it no text");
            return 1;
        }
    }
    Value skipped = Value::MakeArray();
    std::vector<std::string> runIds;
    for (const LibraryMedia* media : wanted) {
        std::string reason;
        if (!media->has_audio) {
            reason = "no_audio";
        } else if (!hasTranscript(media->id)) {
            reason = "no_transcript: transcribe_media (verbatim) first";
        }
        if (reason.empty()) {
            runIds.push_back(media->id);
            continue;
        }
        Value item = Value::MakeObject();
        item.Set("media_id", Value::MakeString(media->id));
        item.Set("filename", Value::MakeString(media->filename));
        item.Set("reason", Value::MakeString(reason));
        skipped.Push(std::move(item));
    }
    const MfaAlignmentSettings settings =
        MfaAlignmentSettings::FromEnvironment();
    if (!runIds.empty()) {
        // PRD21 -- refused by name, with every place each missing file was
        // looked for and the variable that points at it.
        const std::string missing = MissingMfaInstallation(settings);
        if (!missing.empty()) {
            return FailCliCommand(kMfaNotFoundErrorName, missing, output, 1);
        }
    }
    // The WAVs and MFA's working files, in the system's temporary
    // directory: off the exFAT volumes projects often sit on, and removed
    // afterwards.
    const std::filesystem::path work = std::filesystem::temp_directory_path() /
                                       ("cutmachine-mfa-" + GenerateUlid());
    const std::filesystem::path reportPath = work / "report.json";
    std::string textsPath;
    Value report = Value::MakeObject();
    if (!runIds.empty()) {
        std::error_code workError;
        std::filesystem::create_directories(work, workError);
        if (workError) {
            output = ErrorJson(EditError::IoError,
                               "cannot create '" + work.string() +
                                   "': " + workError.message());
            return 1;
        }
        if (!texts.AsObject().empty()) {
            textsPath = (work / "texts.json").string();
            std::ofstream(textsPath) << texts.Dump();
        }
        const std::string label =
            runIds.size() == 1 ? "Alignement MFA"
                               : "Alignement MFA (" +
                                     std::to_string(runIds.size()) + " rushes)";
        const std::string packagePath = projectPath;
        const MediaTaskSnapshot task = RunCommandMediaTask(
            runner, MediaTaskKind::WordAlignment, label,
            [settings, packagePath, runIds, textsPath, work, reportPath](
                MediaTaskContext& context, std::string& taskError) {
                return RunMfaAlignment(settings, packagePath, runIds, textsPath,
                                       work.string(), reportPath.string(),
                                       context, taskError);
            });
        std::string reportText;
        {
            std::ifstream input(reportPath);
            std::ostringstream buffer;
            buffer << input.rdbuf();
            reportText = buffer.str();
        }
        std::error_code ignored;
        std::filesystem::remove_all(work, ignored);
        if (task.state != MediaTaskState::Succeeded) {
            output = ErrorJson(EditError::IoError,
                               MediaTaskFailure(task, "word alignment"));
            return 1;
        }
        if (!Value::Parse(reportText, report, error) || !report.IsObject()) {
            output = ErrorJson(EditError::IoError,
                               "the alignment left no readable report");
            return 1;
        }
    }
    const auto filenameOf = [&](const std::string& id) {
        for (const LibraryMedia& media : project.rushes)
            if (media.id == id) return media.filename;
        return std::string();
    };
    // What the engine reads now, rush by rush: an alignment whose text no
    // longer matches the transcript is ignored by every reader, so it is not
    // reported as aligned.
    Value aligned = Value::MakeArray();
    if (const Value* entries = report.Find("aligned")) {
        for (const Value& entry : entries->AsArray()) {
            const std::string id = entry.Find("media_id")->AsString();
            Transcript words;
            std::string loadError;
            const bool loaded = LoadSourceWords(cache, id, words, loadError);
            Value item = Value::MakeObject();
            item.Set("media_id", Value::MakeString(id));
            item.Set("filename", Value::MakeString(filenameOf(id)));
            item.Set("words", Value::MakeInt(static_cast<int64_t>(
                                  loaded ? words.words.size() : 0)));
            item.Set("word_timing",
                     Value::MakeString(loaded ? words.word_timing : "none"));
            aligned.Push(std::move(item));
        }
    }
    if (const Value* entries = report.Find("skipped")) {
        for (const Value& entry : entries->AsArray()) {
            Value item = entry;
            const std::string id = entry.Find("media_id")->AsString();
            item.Set("filename", Value::MakeString(filenameOf(id)));
            skipped.Push(std::move(item));
        }
    }
    Value uncovered = Value::MakeArray();
    if (const Value* entries = report.Find("uncovered_speech")) {
        for (const Value& entry : entries->AsArray()) {
            Value item = entry;
            item.Set("filename", Value::MakeString(filenameOf(
                                     entry.Find("media_id")->AsString())));
            uncovered.Push(std::move(item));
        }
    }
    Value root = Value::MakeObject();
    root.Set("ok", Value::MakeBool(true));
    root.Set("aligned", std::move(aligned));
    root.Set("skipped", std::move(skipped));
    root.Set("uncovered_speech", std::move(uncovered));
    if (const Value* milliseconds = report.Find("mfa_ms"))
        root.Set("mfa_ms", *milliseconds);
    output = root.Dump() + "\n";
    return 0;
}

int AnalyzeSpeechOnsetCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output,
                              const SpeechOnsetSettings& settings,
                              const MediaTaskRunner& runner,
                              const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    if (media == project.rushes.end()) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (!media->has_audio) {
        output = ErrorJson(EditError::ValidationFailed,
                           "media '" + media->filename + "' has no audio");
        return 1;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path reportPath =
        base / ".cutmachine" / "speechonset" / (mediaId + ".json");
    // IA5 -- through the runner, as the transcriptions.
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::SpeechOnset, "Analyse parole " + media->filename,
        [input, reportPath, mediaId, settings](MediaTaskContext& context,
                                               std::string& taskError) {
            return GenerateSpeechOnset(input.string(), reportPath.string(),
                                       mediaId, settings, context, taskError);
        });
    if (task.state != MediaTaskState::Succeeded) {
        output = ErrorJson(EditError::IoError,
                           MediaTaskFailure(task, "speech onset analysis"));
        return 1;
    }
    SpeechOnsetReport report;
    std::string loadError;
    if (!LoadSpeechOnset(reportPath.string(), report, loadError)) {
        output = ErrorJson(EditError::IoError, loadError);
        return 1;
    }
    bool transcriptAssessed = false;
    TranscriptSpeechAssessment assessment;
    const std::filesystem::path transcriptPath =
        base / ".cutmachine" / "transcripts" / (mediaId + ".json");
    std::error_code transcriptExistsError;
    if (std::filesystem::exists(transcriptPath, transcriptExistsError) &&
        !transcriptExistsError) {
        Transcript transcript;
        if (!LoadAudioTranscript(transcriptPath.string(), transcript,
                                 loadError) ||
            !AssessTranscriptAgainstSpeech(transcript, report, assessment,
                                           loadError)) {
            output = ErrorJson(EditError::IoError,
                               "cannot assess cached transcript: " + loadError);
            return 1;
        }
        ApplyTranscriptSpeechAssessment(transcript, assessment);
        if (!SaveAudioTranscript(transcriptPath.string(), transcript,
                                 loadError)) {
            output = ErrorJson(
                EditError::IoError,
                "cannot save transcript speech assessment: " + loadError);
            return 1;
        }
        transcriptAssessed = true;
    }
    output =
        "{\"ok\":true,\"media_id\":\"" + EscapeJson(mediaId) +
        "\",\"path\":\"" + EscapeJson(reportPath.string()) +
        "\",\"windows\":" + std::to_string(report.levels.size()) +
        ",\"groups\":" + std::to_string(report.groups.size()) +
        ",\"speech_level\":" + std::to_string(report.speech_level) +
        ",\"noise_floor\":" + std::to_string(report.noise_floor) +
        ",\"transcript_assessed\":" + (transcriptAssessed ? "true" : "false");
    if (transcriptAssessed) {
        output +=
            ",\"transcript_words\":" + std::to_string(assessment.word_count) +
            ",\"measured_speech_duration\":{\"value\":" +
            std::to_string(assessment.measured_speech_duration.value) +
            ",\"rate\":" +
            std::to_string(assessment.measured_speech_duration.rate) +
            "},\"likely_hallucinated\":" +
            (assessment.likely_hallucinated ? "true" : "false") +
            ",\"likely_incomplete\":" +
            (assessment.likely_incomplete ? "true" : "false") +
            ",\"known_hallucination_phrase\":" +
            (assessment.known_hallucination_phrase ? "true" : "false");
    }
    output += "}\n";
    return 0;
}

// SYNCHRO-2026-10 SY1 -- docs/roadmap/SYNCHRO-2026-10.md. Read only: both
// rushes' sound decoded at 48 kHz, the channel asked of each, lined up by
// MeasureAudioOffset. Nothing is cached, nothing is written. The drift may
// reach half a frame of the reference's picture, or of the other's when
// the reference has none, or of the active timeline when neither has one.
int MeasureAudioOffsetCommand(
    const std::string& projectPath, const std::string& referenceId,
    const std::string& otherId, const AudioOffsetChannels& channels,
    std::string& output, const MediaTaskRunner& runner, const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const auto find = [&project](const std::string& id) -> const LibraryMedia* {
        for (const LibraryMedia& media : project.rushes) {
            if (media.id == id) {
                return &media;
            }
        }
        return nullptr;
    };
    const LibraryMedia* reference = find(referenceId);
    const LibraryMedia* other = find(otherId);
    if (reference == nullptr || other == nullptr) {
        output =
            ErrorJson(EditError::UnknownMedia,
                      "unknown media_id '" +
                          (reference == nullptr ? referenceId : otherId) + "'");
        return 1;
    }
    if (reference->id == other->id) {
        return FailCliCommand("ValidationFailed",
                              "measure a rush against another one, not itself",
                              output);
    }
    for (const auto& [media, channel] :
         {std::pair{reference, channels.reference},
          std::pair{other, channels.other}}) {
        if (!media->has_audio) {
            return FailCliCommand(
                "ValidationFailed",
                "media '" + media->filename + "' has no audio", output);
        }
        if (channel == AudioOffsetChannel::Right && media->audio_channels < 2) {
            return FailCliCommand("ValidationFailed",
                                  "media '" + media->filename +
                                      "' has one channel: no right to read",
                                  output);
        }
    }
    AudioOffsetSettings settings;
    const auto pictured = [](const LibraryMedia& media) {
        return media.has_video && media.rate.num > 0;
    };
    MediaRate frame = MediaRate{25, 1};
    if (pictured(*reference)) {
        frame = reference->rate;
    } else if (pictured(*other)) {
        frame = other->rate;
    } else if (const DocumentSequence* active = project.ActiveTimeline()) {
        frame = active->frame_rate;
    }
    settings.frame_num = frame.den;
    settings.frame_den = frame.num;
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    const auto pathOf = [&base](const LibraryMedia& media) {
        std::filesystem::path input(media.path);
        if (input.is_relative()) {
            input = base / input;
        }
        return input.lexically_normal().string();
    };
    const std::string referencePath = pathOf(*reference);
    const std::string otherPath = pathOf(*other);
    AudioOffsetReport report;
    std::string refusalCode;
    std::string refusal;
    bool refused = false;
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::AudioOffset,
        "Synchro son " + reference->filename + " / " + other->filename,
        [&](MediaTaskContext& context, std::string& taskError) {
            const auto cancelled = [&context]() { return context.Cancelled(); };
            std::vector<float> referenceSamples;
            std::vector<float> otherSamples;
            context.SetProgress(0.0, "Décodage " + reference->filename);
            if (!DecodeAudioForOffset(referencePath, channels.reference,
                                      settings.sample_rate, cancelled,
                                      referenceSamples, taskError)) {
                return false;
            }
            context.SetProgress(0.4, "Décodage " + other->filename);
            if (!DecodeAudioForOffset(otherPath, channels.other,
                                      settings.sample_rate, cancelled,
                                      otherSamples, taskError)) {
                return false;
            }
            context.SetProgress(0.8, "Mesure du décalage");
            // A refusal is the measure's answer, not the task's failure.
            refused =
                !MeasureAudioOffset(referenceSamples, otherSamples, settings,
                                    report, refusalCode, refusal);
            return true;
        });
    if (task.state != MediaTaskState::Succeeded) {
        return FailCliCommand(
            "IoError", MediaTaskFailure(task, "audio offset measure"), output);
    }
    if (refused) {
        return FailCliCommand(refusalCode, refusal, output);
    }
    mcp_json::Value answer;
    std::string parseError;
    mcp_json::Value::Parse(AudioOffsetReportJson(report, settings), answer,
                           parseError);
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(true));
    result.Set("reference_id", mcp_json::Value::MakeString(reference->id));
    result.Set("other_id", mcp_json::Value::MakeString(other->id));
    for (const auto& [key, value] : answer.AsObject()) {
        result.Set(key, value);
    }
    output = result.Dump() + "\n";
    return 0;
}

// FC6 -- FILMCRAFT-2026-10. Read only, like --measure-audio-offset: the
// rush's sound decoded at 48 kHz on the channel asked, then analysed
// (AudioDspDucking.h). Nothing is cached and nothing is written.
int ProposeDuckingCommand(const std::string& projectPath,
                          const std::string& mediaId,
                          AudioOffsetChannel channel,
                          const audio_dsp::DuckingSettings& settings,
                          std::string& output, const MediaTaskRunner& runner,
                          const Project* stored) {
    Project parsed;
    std::string error;
    if (stored == nullptr && !LoadStoredProject(projectPath, parsed, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Project& project = stored != nullptr ? *stored : parsed;
    const LibraryMedia* media = nullptr;
    for (const LibraryMedia& candidate : project.rushes) {
        if (candidate.id == mediaId) {
            media = &candidate;
        }
    }
    if (media == nullptr) {
        output = ErrorJson(EditError::UnknownMedia,
                           "unknown media_id '" + mediaId + "'");
        return 1;
    }
    if (!media->has_audio) {
        return FailCliCommand("ValidationFailed",
                              "media '" + media->filename + "' has no audio",
                              output);
    }
    if (channel == AudioOffsetChannel::Right && media->audio_channels < 2) {
        return FailCliCommand(
            "ValidationFailed",
            "media '" + media->filename + "' has one channel: no right to read",
            output);
    }
    constexpr int32_t kRate = audio_dsp::kDuckingSampleRate;
    // Before the decode, not after an hour of sound.
    std::string invalid;
    if (!audio_dsp::ValidateDuckingSettings(settings, kRate, invalid)) {
        return FailCliCommand("ValidationFailed", invalid, output);
    }
    std::filesystem::path input(media->path);
    if (input.is_relative()) {
        input = std::filesystem::absolute(projectPath).parent_path() / input;
    }
    const std::string path = input.lexically_normal().string();
    audio_dsp::DuckingProposal proposal;
    std::string refusalCode;
    std::string refusal;
    bool refused = false;
    const MediaTaskSnapshot task = RunCommandMediaTask(
        runner, MediaTaskKind::Ducking, "Ducking " + media->filename,
        [&](MediaTaskContext& context, std::string& taskError) {
            const auto cancelled = [&context]() { return context.Cancelled(); };
            std::vector<float> samples;
            context.SetProgress(0.0, "Décodage " + media->filename);
            if (!DecodeAudioForOffset(path, channel, kRate, cancelled, samples,
                                      taskError)) {
                return false;
            }
            context.SetProgress(0.8, "Plages parlées");
            // A refusal is the analysis's answer, not the task's failure.
            refused = !audio_dsp::ProposeDucking(
                samples, kRate, settings, proposal, refusalCode, refusal);
            return true;
        });
    if (task.state != MediaTaskState::Succeeded) {
        return FailCliCommand(
            "IoError", MediaTaskFailure(task, "ducking analysis"), output);
    }
    if (refused) {
        return FailCliCommand(refusalCode, refusal, output);
    }
    mcp_json::Value answer;
    std::string parseError;
    mcp_json::Value::Parse(audio_dsp::DuckingProposalJson(proposal, settings),
                           answer, parseError);
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(true));
    result.Set("media_id", mcp_json::Value::MakeString(media->id));
    result.Set(
        "channel",
        mcp_json::Value::MakeString(
            channel == AudioOffsetChannel::Mix
                ? "mix"
                : (channel == AudioOffsetChannel::Left ? "left" : "right")));
    for (const auto& [key, value] : answer.AsObject()) {
        result.Set(key, value);
    }
    output = result.Dump() + "\n";
    return 0;
}

namespace {

// SY2 -- the two commands' common course: the offset measured unless given,
// the batch built by `build`, applied, and the clip answered with the
// measure. `soundSourceId` is the recorder's media.
int RunSeparateSoundCommand(
    const std::string& projectPath, const Project& project,
    const DocumentClip& picture, const std::string& soundSourceId,
    const SyncAudioOptions& options,
    const std::function<bool(const RationalTime&, BatchOperation&,
                             SeparateSoundPlacement&, EditError&,
                             std::string&)>& build,
    std::string& output) {
    RationalTime offset = options.offset;
    mcp_json::Value measure;
    if (!options.has_offset) {
        std::string measured;
        if (MeasureAudioOffsetCommand(projectPath, picture.source_id,
                                      soundSourceId, options.channels, measured,
                                      {}, &project) != 0) {
            output = measured;
            return 1;
        }
        std::string parseError;
        int64_t value = 0;
        int64_t rate = 0;
        const bool parsed =
            mcp_json::Value::Parse(measured, measure, parseError);
        const mcp_json::Value* found =
            parsed ? measure.Find("offset") : nullptr;
        const mcp_json::Value* offsetValue =
            found != nullptr ? found->Find("value") : nullptr;
        const mcp_json::Value* offsetRate =
            found != nullptr ? found->Find("rate") : nullptr;
        if (offsetValue == nullptr || offsetRate == nullptr ||
            !offsetValue->AsInt64(value) || !offsetRate->AsInt64(rate)) {
            return FailCliCommand(
                "IoError", "the measure gave no offset: " + measured, output);
        }
        offset = {value, static_cast<int32_t>(rate)};
    }
    BatchOperation batch;
    SeparateSoundPlacement placement;
    EditError error = EditError::None;
    std::string message;
    if (!build(offset, batch, placement, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    int result = 0;
    if (batch.operations.empty()) {
        // Already in sync, linked, the camera's track off.
        output = "{\"ok\":true,\"unchanged\":true}\n";
    } else {
        result = ApplyOperationCommand(projectPath, SerializeOperation(batch),
                                       output);
    }
    mcp_json::Value answer;
    std::string parseError;
    if (result == 0 && mcp_json::Value::Parse(output, answer, parseError) &&
        answer.IsObject()) {
        const auto time = [](const RationalTime& value) {
            mcp_json::Value object = mcp_json::Value::MakeObject();
            object.Set("value", mcp_json::Value::MakeInt(value.value));
            object.Set("rate", mcp_json::Value::MakeInt(value.rate));
            return object;
        };
        mcp_json::Value placed = mcp_json::Value::MakeObject();
        placed.Set("clip_id", mcp_json::Value::MakeString(placement.clip_id));
        placed.Set("source_in", time(placement.source_in));
        placed.Set("duration", time(placement.duration));
        placed.Set("timeline_in", time(placement.timeline_in));
        placed.Set("muted_track_id",
                   placement.muted_track_id.empty()
                       ? mcp_json::Value::MakeNull()
                       : mcp_json::Value::MakeString(placement.muted_track_id));
        answer.Set("placed", std::move(placed));
        if (measure.IsObject()) {
            answer.Set("measure", std::move(measure));
        }
        output = answer.Dump() + "\n";
    }
    return result;
}

}  // namespace

// SY2, the window -- the recorder's sound already on the timeline, slid
// into sync with the picture and linked (BuildSeparateSoundAlign): the edit
// the window's Auto Align applies, on the command line.
int SyncAudioClipCommand(const std::string& projectPath,
                         const std::string& clipId,
                         const std::string& soundClipId,
                         const SyncAudioOptions& options, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* picture = document.FindClip(clipId);
    const DocumentClip* sound = document.FindClip(soundClipId);
    if (picture == nullptr || sound == nullptr) {
        output = ErrorJson(EditError::UnknownClip,
                           "unknown clip_id '" +
                               (picture == nullptr ? clipId : soundClipId) +
                               "' on the active timeline");
        return 1;
    }
    const auto build = [&](const RationalTime& offset, BatchOperation& batch,
                           SeparateSoundPlacement& placement, EditError& error,
                           std::string& text) {
        return BuildSeparateSoundAlign(
            document, {clipId, soundClipId, offset, options.mute_camera_sound},
            batch, placement, error, text);
    };
    // Refused by name before any sound is decoded.
    if (CheckSeparateSoundPair(document, clipId, soundClipId) !=
        SeparateSoundPairFault::None) {
        BatchOperation batch;
        SeparateSoundPlacement placement;
        EditError error = EditError::None;
        build({0, 1}, batch, placement, error, message);
        output = ErrorJson(error, message);
        return 1;
    }
    return RunSeparateSoundCommand(projectPath, project, *picture,
                                   sound->source_id, options, build, output);
}

int SyncAudioCommand(const std::string& projectPath, const std::string& clipId,
                     const std::string& mediaId, const std::string& trackId,
                     const SyncAudioOptions& options, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* picture = document.FindClip(clipId);
    if (picture == nullptr) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return 1;
    }
    return RunSeparateSoundCommand(
        projectPath, project, *picture, mediaId, options,
        [&](const RationalTime& offset, BatchOperation& batch,
            SeparateSoundPlacement& placement, EditError& error,
            std::string& text) {
            return BuildSeparateSoundSync(
                document,
                {clipId, mediaId, trackId, offset, options.mute_camera_sound},
                batch, placement, error, text);
        },
        output);
}

int SpeechOnsetReportCommand(const std::string& projectPath,
                             std::string& output,
                             const SpeechOnsetThresholds& thresholds) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    std::map<Ulid, SpeechOnsetReport> reports;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (reports.count(clip.source_id)) continue;
            SpeechOnsetReport report;
            std::string loadError;
            const std::filesystem::path path = base / ".cutmachine" /
                                               "speechonset" /
                                               (clip.source_id + ".json");
            if (LoadSpeechOnset(path.string(), report, loadError))
                reports.emplace(clip.source_id, std::move(report));
        }
    }
    output = DescribeSpeechOnsetForAgent(document, reports, thresholds) + "\n";
    return 0;
}

namespace {

// Both word commands need the same three things resolved from a clip id, and
// getting any of them wrong is the difference between cutting the right
// frames and cutting someone else's. Kept in one place so neither command
// can drift from the other.
bool LoadClipTranscript(const std::string& projectPath,
                        const std::string& clipId, Project& project,
                        Document& document, const DocumentClip*& clip,
                        Transcript& transcript, std::string& output) {
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return false;
    }
    document = project.MakeActiveDocument();
    clip = document.FindClip(clipId);
    if (clip == nullptr) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return false;
    }
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    if (!LoadSourceWords(base / ".cutmachine", clip->source_id, transcript,
                         message)) {
        output = ErrorJson(EditError::IoError,
                           "no usable transcript for source '" +
                               clip->source_id + "': " + message);
        return false;
    }
    return true;
}

std::string DisfluencyKindName(DisfluencyKind kind) {
    return kind == DisfluencyKind::Filler ? "Filler" : "Repetition";
}

}  // namespace

int ListDisfluenciesCommand(const std::string& projectPath,
                            const std::string& clipId, std::string& output) {
    Project project;
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!LoadClipTranscript(projectPath, clipId, project, document, clip,
                            transcript, output))
        return 1;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    std::ostringstream json;
    json << "{\"ok\":true,\"clip_id\":\"" << EscapeJson(clip->id)
         << "\",\"source_id\":\"" << EscapeJson(clip->source_id)
         << "\",\"whisper_model\":\""
         << EscapeJson(transcript.whisper_model)
         // Reported because it decides what this list can possibly contain:
         // Whisper's default decoding drops most fillers before they are ever
         // written down, so an empty result on a non-verbatim transcript says
         // nothing about how the take was actually spoken.
         << "\",\"verbatim\":" << (transcript.verbatim ? "true" : "false");
    // ITW9 -- the words a plain pass gave a likely_incomplete verbatim one
    // carry no hesitation the verbatim pass did not keep.
    if (transcript.plain_pass_words > 0) {
        json << ",\"plain_pass_words\":" << transcript.plain_pass_words;
    }
    json << ",\"words\":" << transcript.words.size() << ",\"disfluencies\":[";
    for (size_t index = 0; index < found.size(); ++index) {
        const Disfluency& item = found[index];
        if (index != 0) json << ',';
        json << "{\"start_word_index\":" << item.range.start_word_index
             << ",\"end_word_index\":" << item.range.end_word_index
             << ",\"kind\":\"" << DisfluencyKindName(item.kind)
             << "\",\"text\":\"" << EscapeJson(item.text) << "\"}";
    }
    json << "]}\n";
    output = json.str();
    return 0;
}

int RemoveWordsCommand(const std::string& projectPath,
                       const std::string& clipId, const std::string& rangesJson,
                       std::string& output) {
    Project project;
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!LoadClipTranscript(projectPath, clipId, project, document, clip,
                            transcript, output))
        return 1;

    mcp_json::Value parsed;
    std::string message;
    if (!mcp_json::Value::Parse(rangesJson, parsed, message) ||
        !parsed.IsArray()) {
        output = ErrorJson(EditError::ParseError,
                           "word ranges must be a JSON array: " + message);
        return 1;
    }
    std::vector<WordRange> ranges;
    for (const mcp_json::Value& value : parsed.AsArray()) {
        const mcp_json::Value* start = value.Find("start_word_index");
        const mcp_json::Value* end = value.Find("end_word_index");
        int64_t startIndex = 0;
        int64_t endIndex = 0;
        if (start == nullptr || end == nullptr || !start->AsInt64(startIndex) ||
            !end->AsInt64(endIndex) || startIndex < 0 || endIndex < 0) {
            output = ErrorJson(
                EditError::InvalidOperation,
                "each word range needs integer 'start_word_index' and "
                "'end_word_index'");
            return 1;
        }
        ranges.push_back(
            {static_cast<size_t>(startIndex), static_cast<size_t>(endIndex)});
    }

    // The caller only ever names words. Turning those into frames is
    // ResolveWordRemoval's job and nobody else's -- see Transcription.h,
    // where it is documented as the only intended producer of a
    // WordRemovalRange.
    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, ranges, RationalTime{0, 1}, {},
                            operation, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    // ITW7 -- the same pair and followers as the MCP tool: this command cut
    // the picture alone and moved no other track.
    AttachLinkedRemovals(document, *clip, operation);
    operation.sync_track_ids = WordRemovalSyncTrackIds(document, operation);
    // Handing the serialized operation back to ApplyOperationCommand keeps
    // this on the one reversible path: same edit log, same undo, same
    // round-trip guarantee as any other edit. The words named are the ones
    // its `cut_words` counts as targeted; any other is carried off.
    return ApplyOperationCommand(projectPath, SerializeOperation(operation),
                                 output, {}, {{clip->source_id, ranges}});
}

int TightenPausesCommand(const std::string& projectPath,
                         const std::string& clipId, int64_t minimumGapMs,
                         int64_t keepFrames, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return 1;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr) {
        output =
            ErrorJson(EditError::UnknownSource, "clip '" + clipId +
                                                    "' reads from an unmounted "
                                                    "source");
        return 1;
    }
    const std::filesystem::path envelopePath =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "speechonset" / (clip->source_id + ".json");
    SpeechOnsetReport envelope;
    if (!LoadSpeechOnset(envelopePath.string(), envelope, message)) {
        output = ErrorJson(EditError::IoError,
                           "no speech envelope for source '" + clip->source_id +
                               "': " + message +
                               " (run --speech-onset on it first)");
        return 1;
    }

    PauseTighteningSettings settings;
    settings.minimum_gap_milliseconds = minimumGapMs;
    settings.keep_frames = keepFrames;
    // IA8 -- the same reader the word tools use: Whisper's cache with the
    // newest matching alignment applied. Absent, the pauses close unguarded
    // and the report says the words went unchecked.
    Transcript words;
    std::string wordsError;
    const bool haveWords = LoadSourceWords(
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine",
        clip->source_id, words, wordsError);
    RemoveWordsOperation operation;
    PauseTighteningReport report;
    if (!ResolvePauseTightening(*clip, envelope, haveWords ? &words : nullptr,
                                settings, source->rate, {}, operation, report,
                                message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    if (operation.ranges.empty()) {
        // Nothing to close is a success: "tighten this clip" on an already
        // tight clip has been honoured. Applying an empty RemoveWords would
        // only be refused by the engine.
        output = "{\"ok\":true,\"clip_id\":\"" + EscapeJson(clip->id) +
                 "\",\"applied\":false,\"report\":" +
                 SerializePauseTighteningReport(report) + "}\n";
        return 0;
    }
    AttachLinkedRemovals(document, *clip, operation);
    operation.sync_track_ids = WordRemovalSyncTrackIds(document, operation);
    // Same hand-off RemoveWordsCommand makes: the serialized operation goes
    // through ApplyOperationCommand, so this lands on the one reversible
    // path with the same edit log and the same undo as any other edit.
    const int result = ApplyOperationCommand(
        projectPath, SerializeOperation(operation), output);
    if (result != 0) return result;
    // The apply result is the document-facing half; the report is what says
    // which pauses were closed and what was deliberately left standing. Both
    // are carried back, so one call answers "what changed" and "why" --
    // parsed and re-emitted rather than spliced, so a change to the shape of
    // either payload cannot silently produce invalid JSON.
    mcp_json::Value applied;
    mcp_json::Value reportView;
    std::string parseError;
    if (mcp_json::Value::Parse(output, applied, parseError) &&
        applied.IsObject() &&
        mcp_json::Value::Parse(SerializePauseTighteningReport(report),
                               reportView, parseError)) {
        applied.Set("report", std::move(reportView));
        output = applied.Dump() + "\n";
    }
    return 0;
}

int TrimBoundaryAirCommand(const std::string& projectPath,
                           const std::string& clipId, int64_t keepFrames,
                           int64_t minimumAirMs, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip) {
        output =
            ErrorJson(EditError::UnknownClip, "unknown clip_id '" + clipId +
                                                  "' on the active timeline");
        return 1;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (!source) {
        output =
            ErrorJson(EditError::UnknownSource,
                      "clip '" + clipId + "' reads from an unmounted source");
        return 1;
    }
    const std::filesystem::path envelopePath =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "speechonset" / (clip->source_id + ".json");
    SpeechOnsetReport envelope;
    if (!LoadSpeechOnset(envelopePath.string(), envelope, message)) {
        output = ErrorJson(EditError::IoError,
                           "no speech envelope for source '" + clip->source_id +
                               "': " + message +
                               " (run --speech-onset on it first)");
        return 1;
    }

    BoundaryAirSettings settings;
    settings.keep_frames = keepFrames;
    settings.minimum_air_milliseconds = minimumAirMs;
    TrimBoundaryAirOperation operation;
    BoundaryAirReport report;
    if (!ResolveBoundaryAir(*clip, envelope, settings, source->rate, {},
                            operation, report, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    for (BoundaryAirTrim& trim : operation.trims) {
        const DocumentClip* target = document.FindClip(trim.clip_id);
        if (target)
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, *target, trim.edge);
    }
    operation.sync_track_ids = BoundarySyncTrackIds(document, operation.trims);

    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("ok", mcp_json::Value::MakeBool(true));
    root.Set("clip_id", mcp_json::Value::MakeString(clip->id));
    mcp_json::Value reportView;
    std::string parseError;
    if (mcp_json::Value::Parse(SerializeBoundaryAirReport(report), reportView,
                               parseError)) {
        root.Set("report", std::move(reportView));
    }
    if (operation.trims.empty()) {
        root.Set("applied", mcp_json::Value::MakeBool(false));
        output = root.Dump() + "\n";
        return 0;
    }
    std::string appliedJson;
    const int result = ApplyOperationCommand(
        projectPath, SerializeOperation(operation), appliedJson);
    if (result != 0) {
        output = std::move(appliedJson);
        return result;
    }
    root.Set("applied", mcp_json::Value::MakeBool(true));
    mcp_json::Value applied;
    if (mcp_json::Value::Parse(appliedJson, applied, parseError) &&
        applied.IsObject()) {
        const mcp_json::Value* hash = applied.Find("doc_hash");
        if (hash) root.Set("doc_hash", *hash);
        // IA9 -- a boundary trim is a cut like any other.
        const mcp_json::Value* words = applied.Find("cut_words");
        if (words) root.Set("cut_words", *words);
    }
    output = root.Dump() + "\n";
    return 0;
}

int CloseJunctionAirCommand(const std::string& projectPath,
                            const std::string& leftClipId,
                            const std::string& rightClipId, int64_t keepFrames,
                            int64_t minimumAirMs, std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    const DocumentClip* left = document.FindClip(leftClipId);
    const DocumentClip* right = document.FindClip(rightClipId);
    if (!left || !right) {
        output = ErrorJson(EditError::UnknownClip,
                           "unknown clip in junction '" + leftClipId +
                               "' -> '" + rightClipId + "'");
        return 1;
    }
    const DocumentTrack* leftTrack = document.FindTrackForClip(left->id);
    const DocumentTrack* rightTrack = document.FindTrackForClip(right->id);
    if (!leftTrack || !rightTrack || leftTrack->id != rightTrack->id) {
        output = ErrorJson(EditError::InvalidOperation,
                           "junction clips must be on the same track");
        return 1;
    }
    const DocumentSource* leftSource = document.FindSource(left->source_id);
    const DocumentSource* rightSource = document.FindSource(right->source_id);
    if (!leftSource || !rightSource) {
        output = ErrorJson(EditError::UnknownSource,
                           "junction clip reads from an unmounted source");
        return 1;
    }
    const std::filesystem::path cacheRoot =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "speechonset";
    SpeechOnsetReport leftEnvelope;
    SpeechOnsetReport rightEnvelope;
    if (!LoadSpeechOnset((cacheRoot / (left->source_id + ".json")).string(),
                         leftEnvelope, message) ||
        !LoadSpeechOnset((cacheRoot / (right->source_id + ".json")).string(),
                         rightEnvelope, message)) {
        output = ErrorJson(EditError::IoError,
                           "missing speech envelope for junction: " + message +
                               " (run --speech-onset first)");
        return 1;
    }

    BoundaryAirSettings settings;
    settings.keep_frames = keepFrames;
    settings.minimum_air_milliseconds = minimumAirMs;
    TrimBoundaryAirOperation operation;
    JunctionAirReport report;
    if (!ResolveJunctionAir(*left, leftEnvelope, leftSource->rate, *right,
                            rightEnvelope, rightSource->rate, settings, {},
                            operation, report, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    for (BoundaryAirTrim& trim : operation.trims) {
        const DocumentClip* target = document.FindClip(trim.clip_id);
        if (target)
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, *target, trim.edge);
    }
    operation.sync_track_ids = BoundarySyncTrackIds(document, operation.trims);

    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("ok", mcp_json::Value::MakeBool(true));
    root.Set("left_clip_id", mcp_json::Value::MakeString(left->id));
    root.Set("right_clip_id", mcp_json::Value::MakeString(right->id));
    mcp_json::Value reportView;
    std::string parseError;
    if (mcp_json::Value::Parse(SerializeJunctionAirReport(report), reportView,
                               parseError)) {
        root.Set("report", std::move(reportView));
    }
    if (operation.trims.empty()) {
        root.Set("applied", mcp_json::Value::MakeBool(false));
        output = root.Dump() + "\n";
        return 0;
    }
    std::string appliedJson;
    const int result = ApplyOperationCommand(
        projectPath, SerializeOperation(operation), appliedJson);
    if (result != 0) {
        output = std::move(appliedJson);
        return result;
    }
    root.Set("applied", mcp_json::Value::MakeBool(true));
    mcp_json::Value applied;
    if (mcp_json::Value::Parse(appliedJson, applied, parseError) &&
        applied.IsObject()) {
        const mcp_json::Value* hash = applied.Find("doc_hash");
        if (hash) root.Set("doc_hash", *hash);
        // IA9 -- a boundary trim is a cut like any other.
        const mcp_json::Value* words = applied.Find("cut_words");
        if (words) root.Set("cut_words", *words);
    }
    output = root.Dump() + "\n";
    return 0;
}

int LocateSourceFrameCommand(const std::string& projectPath,
                             const std::string& mediaId, int64_t sourceFrame,
                             std::string& output) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Document document = project.MakeActiveDocument();
    std::vector<SourceFrameMatch> matches;
    if (!ResolveSourceFrame(document, mediaId, sourceFrame, matches, message)) {
        output = ErrorJson(EditError::UnknownMedia, message);
        return 1;
    }
    // PRD39 -- locate_source_frame's answer, with the command line's "ok".
    output = WithCliOk(
        DescribeSourceFrameMatches(mediaId, sourceFrame, matches) + "\n");
    return 0;
}

int DescribeCommand(const std::string& documentPath, std::string& output,
                    const std::string& timelineId) {
    std::string message;
    Project project;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    if (!timelineId.empty() && project.FindTimeline(timelineId) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + timelineId + "'");
        return 1;
    }
    try {
        // PRD39 -- the snapshot the describe tool reads (DescribeProject),
        // with the command line's "ok" in front; the tool's answer has
        // none, as before.
        output = WithCliOk(DescribeProject(project, timelineId));
        return 0;
    } catch (const std::exception& exception) {
        output = ErrorJson(EditError::ArithmeticError, exception.what());
        return 1;
    }
}

int ProposeSequenceCommand(const std::string& projectPath,
                           std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        output = ErrorJson(EditError::ParseError, error);
        return 1;
    }
    SequenceFormatProposal proposal;
    if (!ResolveSequenceFormat(project.rushes, proposal, error)) {
        output = ErrorJson(EditError::InvalidOperation, error);
        return 1;
    }
    output = SequenceFormatProposalJson(proposal) + "\n";
    return 0;
}

namespace {

bool LoadOptionalTimelineLog(const std::string& projectPath,
                             const std::string& timelineId, EditLog& log,
                             EditError& error, std::string& message) {
    std::string path = TimelineEditLogPathForProject(projectPath, timelineId);
    std::error_code existsError;
    bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = EditError::IoError;
        message = "unable to inspect edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    if (!exists) return true;
    std::string json;
    return ReadFile(path, json, message) &&
           EditLog::Deserialize(json, log, error, message);
}

bool LoadOptionalProjectLog(const std::string& projectPath, ProjectEditLog& log,
                            EditError& error, std::string& message) {
    const std::string path = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = EditError::IoError;
        message = "unable to inspect project edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    if (!exists) return true;
    std::string json;
    return ReadFile(path, json, message) &&
           ProjectEditLog::Deserialize(json, log, error, message);
}

}  // namespace

bool LoadProjectEditLogIfAny(const std::string& projectPath,
                             ProjectEditLog& log, EditError& error,
                             std::string& message) {
    return LoadOptionalProjectLog(projectPath, log, error, message);
}

namespace {

// IA9 -- the `cut_words` member of an edit result, or empty when the edit
// took no sound out of the timeline. Words come from the reader every word
// tool uses, so an alignment that would not be applied to a cut is not
// trusted to describe one either.
std::string CutWordsMember(const std::string& projectPath,
                           const Document& before, const Document& after,
                           const TargetedWords& targetedWords) {
    const std::vector<RemovedSourceRange> removed =
        RemovedAudioMaterial(before, after);
    if (removed.empty()) return {};
    const std::filesystem::path cacheDirectory =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine";
    std::map<Ulid, Transcript> transcripts;
    for (const RemovedSourceRange& range : removed) {
        if (transcripts.count(range.source_id)) continue;
        Transcript words;
        std::string ignored;
        if (LoadSourceWords(cacheDirectory, range.source_id, words, ignored))
            transcripts.emplace(range.source_id, std::move(words));
    }
    return ",\"cut_words\":" + SerializeCutWordsReport(CheckCutWords(
                                   removed, transcripts, targetedWords));
}

}  // namespace

int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson, std::string& output,
                          const std::string& timelineId,
                          const TargetedWords& targetedWords,
                          const std::vector<EntryMotif>& motifs) {
    // A malformed operation is named before the package is read, as it
    // always was.
    Operation operation = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeOperation(operationJson, operation, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    Project project;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    return ApplyOperationToLoadedProject(documentPath, project, operationJson,
                                         output, timelineId, targetedWords,
                                         motifs);
}

int ApplyOperationToLoadedProject(const std::string& documentPath,
                                  Project& project,
                                  const std::string& operationJson,
                                  std::string& output,
                                  const std::string& timelineId,
                                  const TargetedWords& targetedWords,
                                  const std::vector<EntryMotif>& motifs) {
    Operation operation = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeOperation(operationJson, operation, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    // What is stored, for the commit to compare with.
    const Project stored = project;
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selectedTimeline + "'");
        return 1;
    }
    Document document = project.MakeDocument(selectedTimeline);
    // DELTA-2026-08 -- the state the caller already knows about. Kept so the
    // result can say what changed instead of making the caller re-read the
    // whole timeline to find out.
    const Document before = document;

    EditLog log;
    if (!LoadOptionalTimelineLog(documentPath, selectedTimeline, log, error,
                                 message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    if (!log.Apply(document, std::move(operation), error, message, motifs)) {
        output = ErrorJson(error, message);
        return 1;
    }

    const std::string updatedDocument = document.SaveToString();
    std::map<std::string, EditLog> logs;
    logs[selectedTimeline] = log;
    // B10 -- ROADMAP.md, extended to the project journal. A timeline edit
    // leaves project-level history untouched, so it has to be read back and
    // rewritten as-is: committing a default-constructed one erased every
    // project undo step on each --apply-op and each MCP tool call.
    ProjectEditLog projectLog;
    if (!LoadOptionalProjectLog(documentPath, projectLog, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    // BINS_SPEC.md -- the project refusing the edit is a refusal, not a
    // failed write: an agent told IoError retries or suspects the disk.
    if (!project.CommitDocument(selectedTimeline, document, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    if (!CommitStoredProjectAndLogs(documentPath, project, logs, projectLog,
                                    message, &stored)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(before, document, delta);
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\",\"delta\":" + SerializeDocumentDelta(delta) +
             CutWordsMember(documentPath, before, document, targetedWords) +
             "}\n";
    return 0;
}

namespace {

// `project` is the package as stored; on success, the project committed.
int MutateLoadedProjectLog(
    const std::string& projectPath, Project& project,
    const std::optional<ProjectOperation>& operation,
    const std::optional<std::vector<ProjectOperation>>& operationBatch,
    bool redo, std::string& output,
    const std::vector<EntryMotif>& motifs = {}) {
    std::string message;
    EditError error = EditError::None;
    // What is stored, for the commit to compare with.
    const Project stored = project;
    ProjectEditLog projectLog;
    if (!LoadOptionalProjectLog(projectPath, projectLog, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    std::vector<Ulid> timelinesBefore;
    for (const DocumentSequence& timeline : project.timelines)
        timelinesBefore.push_back(timeline.id);
    bool changed = false;
    // MOTIF6 -- the call's motif rides on the entry the edit writes.
    if (operation)
        changed = projectLog.Apply(project, *operation, error, message, motifs);
    else if (operationBatch)
        changed = projectLog.ApplyBatch(project, *operationBatch, error,
                                        message, motifs);
    else if (redo)
        changed = projectLog.Redo(project, error, message);
    else
        changed = projectLog.Undo(project, error, message);
    if (!changed) {
        output = ErrorJson(error, message);
        return 1;
    }
    // B10 -- ROADMAP.md. Project operations do not edit timeline histories.
    // An empty partial write set preserves existing journals and creates one
    // only for a newly added timeline in CommitStoredProjectAndLogs.
    const std::map<std::string, EditLog> timelineLogs;
    if (!CommitStoredProjectAndLogs(projectPath, project, timelineLogs,
                                    projectLog, message, &stored)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    // ITW7 -- ROADMAP.md. add_timeline answered with a hash alone, so the
    // timeline just created could only be addressed after list_timelines.
    // Diffed, like DocumentDelta, rather than reported by each operation.
    mcp_json::Value created = mcp_json::Value::MakeArray();
    mcp_json::Value removed = mcp_json::Value::MakeArray();
    for (const DocumentSequence& timeline : project.timelines) {
        if (std::find(timelinesBefore.begin(), timelinesBefore.end(),
                      timeline.id) == timelinesBefore.end())
            created.Push(mcp_json::Value::MakeString(timeline.id));
    }
    for (const Ulid& id : timelinesBefore) {
        if (project.FindTimeline(id) == nullptr)
            removed.Push(mcp_json::Value::MakeString(id));
    }
    output = "{\"ok\":true,\"project_hash\":\"" +
             CanonicalHash(project.SaveToString()) + '"';
    if (!created.AsArray().empty())
        output += ",\"created_timeline_ids\":" + created.Dump();
    if (!removed.AsArray().empty())
        output += ",\"removed_timeline_ids\":" + removed.Dump();
    output += "}\n";
    return 0;
}

int MutateProjectLogCommand(
    const std::string& projectPath,
    const std::optional<ProjectOperation>& operation,
    const std::optional<std::vector<ProjectOperation>>& operationBatch,
    bool redo, std::string& output,
    const std::vector<EntryMotif>& motifs = {}) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    return MutateLoadedProjectLog(projectPath, project, operation,
                                  operationBatch, redo, output, motifs);
}

// One project operation, or an array of them applied as one batch. A
// malformed one is named before the package is read.
bool ParseProjectOperationInput(
    const std::string& operationJson,
    std::optional<ProjectOperation>& operation,
    std::optional<std::vector<ProjectOperation>>& operationBatch,
    std::string& output) {
    mcp_json::Value input;
    std::string jsonError;
    if (mcp_json::Value::Parse(operationJson, input, jsonError) &&
        input.IsArray()) {
        std::vector<ProjectOperation> operations;
        operations.reserve(input.AsArray().size());
        for (size_t index = 0; index < input.AsArray().size(); ++index) {
            ProjectOperation item = AddProjectTimelineOperation{};
            EditError error = EditError::None;
            std::string message;
            if (!DeserializeProjectOperation(input.AsArray()[index].Dump(),
                                             item, error, message)) {
                output = ErrorJson(error, "project operation batch item " +
                                              std::to_string(index) + ": " +
                                              message);
                return false;
            }
            operations.push_back(std::move(item));
        }
        operationBatch = std::move(operations);
        return true;
    }

    ProjectOperation single = AddProjectTimelineOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeProjectOperation(operationJson, single, error, message)) {
        output = ErrorJson(error, message);
        return false;
    }
    operation = std::move(single);
    return true;
}

}  // namespace

int ApplyProjectOperationCommand(const std::string& projectPath,
                                 const std::string& operationJson,
                                 std::string& output,
                                 const std::vector<EntryMotif>& motifs) {
    std::optional<ProjectOperation> operation;
    std::optional<std::vector<ProjectOperation>> operationBatch;
    if (!ParseProjectOperationInput(operationJson, operation, operationBatch,
                                    output)) {
        return 1;
    }
    return MutateProjectLogCommand(projectPath, operation, operationBatch,
                                   false, output, motifs);
}

int ApplyProjectOperationToLoadedProject(
    const std::string& projectPath, Project& project,
    const std::string& operationJson, std::string& output,
    const std::vector<EntryMotif>& motifs) {
    std::optional<ProjectOperation> operation;
    std::optional<std::vector<ProjectOperation>> operationBatch;
    if (!ParseProjectOperationInput(operationJson, operation, operationBatch,
                                    output)) {
        return 1;
    }
    return MutateLoadedProjectLog(projectPath, project, operation,
                                  operationBatch, false, output, motifs);
}

int UndoProjectOperationCommand(const std::string& projectPath,
                                std::string& output) {
    return MutateProjectLogCommand(projectPath, std::nullopt, std::nullopt,
                                   false, output);
}

int RedoProjectOperationCommand(const std::string& projectPath,
                                std::string& output) {
    return MutateProjectLogCommand(projectPath, std::nullopt, std::nullopt,
                                   true, output);
}

namespace {

// Shared tail of UndoOperationCommand/RedoOperationCommand: identical to the
// mutate/commit shape of ApplyOperationToLoadedProject above, but drives the
// active timeline's EditLog::Undo/Redo instead of EditLog::Apply. Kept
// separate from it rather than folded into it so neither entry point grows a
// branch the other doesn't need. `project` is the package as stored; on
// success, the project committed.
int MutateLoadedTimelineLog(const std::string& documentPath, Project& project,
                            bool redo, std::string& output,
                            const std::string& timelineId) {
    std::string message;
    EditError error = EditError::None;
    // What is stored, for the commit to compare with.
    const Project stored = project;
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selectedTimeline + "'");
        return 1;
    }
    Document document = project.MakeDocument(selectedTimeline);
    // Undo needs the delta more than anything else does: the caller has no
    // way to guess what a reversal put back.
    const Document before = document;

    EditLog log;
    if (!LoadOptionalTimelineLog(documentPath, selectedTimeline, log, error,
                                 message)) {
        output = ErrorJson(error, message);
        return 1;
    }

    const bool changed = redo ? log.Redo(document, error, message)
                              : log.Undo(document, error, message);
    if (!changed) {
        output = ErrorJson(error, message);
        return 1;
    }

    const std::string updatedDocument = document.SaveToString();
    std::map<std::string, EditLog> logs;
    logs[selectedTimeline] = log;
    // B10 -- ROADMAP.md, extended to the project journal, for the same
    // reason ApplyOperationCommand above reloads it: undoing a timeline edit
    // must not erase project-level history.
    ProjectEditLog projectLog;
    if (!LoadOptionalProjectLog(documentPath, projectLog, error, message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    // BINS_SPEC.md -- refused, as an edit is (undoing add_bin under a filed
    // timeline), rather than reported as a failed write.
    if (!project.CommitDocument(selectedTimeline, document, message)) {
        output = ErrorJson(EditError::InvalidOperation, message);
        return 1;
    }
    if (!CommitStoredProjectAndLogs(documentPath, project, logs, projectLog,
                                    message, &stored)) {
        output = ErrorJson(EditError::IoError, message);
        return 1;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(before, document, delta);
    output = "{\"ok\":true,\"doc_hash\":\"" + CanonicalHash(updatedDocument) +
             "\",\"delta\":" + SerializeDocumentDelta(delta) + "}\n";
    return 0;
}

int MutateTimelineLogCommand(const std::string& documentPath, bool redo,
                             std::string& output,
                             const std::string& timelineId) {
    Project project;
    std::string message;
    if (!LoadStoredProject(documentPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    return MutateLoadedTimelineLog(documentPath, project, redo, output,
                                   timelineId);
}

}  // namespace

int UndoOperationCommand(const std::string& documentPath, std::string& output,
                         const std::string& timelineId) {
    return MutateTimelineLogCommand(documentPath, false, output, timelineId);
}

int RedoOperationCommand(const std::string& documentPath, std::string& output,
                         const std::string& timelineId) {
    return MutateTimelineLogCommand(documentPath, true, output, timelineId);
}

int UndoOperationOnLoadedProject(const std::string& documentPath,
                                 Project& project, std::string& output,
                                 const std::string& timelineId) {
    return MutateLoadedTimelineLog(documentPath, project, false, output,
                                   timelineId);
}

int RedoOperationOnLoadedProject(const std::string& documentPath,
                                 Project& project, std::string& output,
                                 const std::string& timelineId) {
    return MutateLoadedTimelineLog(documentPath, project, true, output,
                                   timelineId);
}

int BeginInterventionCommand(const std::string& projectPath,
                             std::string& output,
                             const std::string& timelineId) {
    std::string message;
    EditError error = EditError::None;
    Project project;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(selectedTimeline) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + selectedTimeline + "'");
        return 1;
    }
    EditLog log;
    if (!LoadOptionalTimelineLog(projectPath, selectedTimeline, log, error,
                                 message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    output = InterventionBeganJson(MarkIntervention(selectedTimeline, log),
                                   project.MakeDocument(selectedTimeline)) +
             "\n";
    return 0;
}

int EndInterventionCommand(const std::string& projectPath,
                           const std::string& intervention,
                           const std::string& label, std::string& output,
                           const std::optional<EditMotif>& motif) {
    std::string message;
    EditError error = EditError::None;
    InterventionMark mark;
    if (!ParseInterventionMark(intervention, mark, message)) {
        return FailCliCommand("ValidationFailed", message, output);
    }
    Project project;
    if (!LoadStoredProject(projectPath, project, message)) {
        output = ErrorJson(EditError::ParseError, message);
        return 1;
    }
    if (project.FindTimeline(mark.timeline_id) == nullptr) {
        output = ErrorJson(EditError::UnknownSequence,
                           "unknown timeline_id '" + mark.timeline_id + "'");
        return 1;
    }
    const Document document = project.MakeDocument(mark.timeline_id);
    EditLog log;
    if (!LoadOptionalTimelineLog(projectPath, mark.timeline_id, log, error,
                                 message)) {
        output = ErrorJson(error, message);
        return 1;
    }
    size_t grouped = 0;
    if (!log.GroupSince(document, mark, label, grouped, error, message,
                        motif)) {
        output = ErrorJson(error, message);
        return 1;
    }
    if (grouped > 0) {
        // Only the journal changed -- PRD40, for a single step too, which is
        // now labelled. The project journal is read back and rewritten as
        // it is, for the reason ApplyOperationCommand gives.
        std::map<std::string, EditLog> logs;
        logs[mark.timeline_id] = log;
        ProjectEditLog projectLog;
        if (!LoadOptionalProjectLog(projectPath, projectLog, error, message)) {
            output = ErrorJson(error, message);
            return 1;
        }
        if (!CommitStoredProjectAndLogs(projectPath, project, logs, projectLog,
                                        message)) {
            output = ErrorJson(EditError::IoError, message);
            return 1;
        }
    }
    output = InterventionEndedJson(grouped, log, document) + "\n";
    return 0;
}

namespace {

// PL3 -- the session one plan command opens: the project under its lock,
// as --call-tool holds it, and a catalog whose plan store lives as long as
// the command does.
bool OpenPlanSession(const std::string& projectPath, McpProjectBackend& backend,
                     std::string& output) {
    std::string opened;
    std::string errorName;
    std::string message;
    if (!backend.OpenProject(
            std::filesystem::absolute(projectPath).lexically_normal().string(),
            opened, errorName, message)) {
        FailCliCommand(errorName, message, output);
        return false;
    }
    return true;
}

bool ReadEditPlanFile(const std::string& planPath, EditPlan& plan,
                      std::string& output) {
    std::string json;
    std::string message;
    if (!ReadFile(planPath, json, message)) {
        FailCliCommand("IoError", message, output);
        return false;
    }
    if (!ParseEditPlan(json, plan, message)) {
        FailCliCommand("ValidationFailed", "'" + planPath + "': " + message,
                       output);
        return false;
    }
    return true;
}

// Adds `plan_file` to a JSON object answer.
std::string WithPlanFile(const std::string& answer,
                         const std::string& planPath) {
    mcp_json::Value value;
    std::string parseError;
    if (!mcp_json::Value::Parse(answer, value, parseError) ||
        !value.IsObject()) {
        return answer;
    }
    value.Set("plan_file", mcp_json::Value::MakeString(planPath));
    return value.Dump();
}

}  // namespace

int PrepareEditPlanCommand(const std::string& projectPath,
                           const std::string& argumentsJson,
                           const std::string& planPath, std::string& output,
                           bool requireExplicitTimeline) {
    mcp_json::Value arguments;
    std::string error;
    if (!mcp_json::Value::Parse(argumentsJson, arguments, error)) {
        return FailCliCommand("ParseError", error, output, 2);
    }
    if (planPath.empty()) {
        return FailCliCommand("ValidationFailed",
                              "--prepare-edit-plan needs the path of the plan "
                              "file to write",
                              output, 2);
    }
    std::error_code existsError;
    if (std::filesystem::exists(planPath, existsError) || existsError) {
        return FailCliCommand(
            "ValidationFailed",
            "'" + planPath +
                "' already exists: a plan file is never overwritten; "
                "discard it with --discard-edit-plan or name another file",
            output);
    }
    McpProjectBackend backend({}, requireExplicitTimeline);
    if (!OpenPlanSession(projectPath, backend, output)) {
        return 1;
    }
    const McpToolRegistry registry;
    const McpToolCallOutcome outcome =
        registry.Call(backend, "prepare_edit_plan", arguments);
    if (!outcome.ok) {
        output = outcome.result_json + '\n';
        return 1;
    }
    mcp_json::Value result;
    const mcp_json::Value* planId =
        mcp_json::Value::Parse(outcome.result_json, result, error)
            ? result.Find("plan_id")
            : nullptr;
    EditPlan plan;
    if (planId == nullptr || !planId->IsString() ||
        !registry.EditPlans().Find(planId->AsString(), plan)) {
        return FailCliCommand(
            "IoError", "prepare_edit_plan did not keep its plan", output);
    }
    if (!WriteFile(planPath, SerializeEditPlan(plan), error)) {
        return FailCliCommand("IoError", error, output);
    }
    output = WithPlanFile(outcome.result_json, planPath) + '\n';
    return 0;
}

int ApplyEditPlanCommand(const std::string& projectPath,
                         const std::string& planPath, std::string& output,
                         bool requireExplicitTimeline) {
    EditPlan plan;
    if (!ReadEditPlanFile(planPath, plan, output)) {
        return 1;
    }
    McpProjectBackend backend({}, requireExplicitTimeline);
    if (!OpenPlanSession(projectPath, backend, output)) {
        return 1;
    }
    const McpToolRegistry registry;
    std::string errorName;
    std::string message;
    const Ulid planId = plan.plan_id;
    if (!registry.EditPlans().Add(std::move(plan), errorName, message)) {
        return FailCliCommand(errorName, message, output);
    }
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("plan_id", mcp_json::Value::MakeString(planId));
    const McpToolCallOutcome outcome =
        registry.Call(backend, "apply_edit_plan", arguments);
    if (!outcome.ok) {
        output = outcome.result_json + '\n';
        return 1;
    }
    output = WithPlanFile(outcome.result_json, planPath) + '\n';
    return 0;
}

int DiscardEditPlanCommand(const std::string& planPath, std::string& output) {
    EditPlan plan;
    if (!ReadEditPlanFile(planPath, plan, output)) {
        return 1;
    }
    std::error_code removeError;
    if (!std::filesystem::remove(planPath, removeError) || removeError) {
        return FailCliCommand(
            "IoError",
            "unable to remove '" + planPath + "': " + removeError.message(),
            output);
    }
    mcp_json::Value answer = mcp_json::Value::MakeObject();
    answer.Set("ok", mcp_json::Value::MakeBool(true));
    answer.Set("status", mcp_json::Value::MakeString("discarded"));
    answer.Set("plan_id", mcp_json::Value::MakeString(plan.plan_id));
    answer.Set("plan_file", mcp_json::Value::MakeString(planPath));
    output = answer.Dump() + '\n';
    return 0;
}
