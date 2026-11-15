#pragma once

#include "AudioDspDucking.h"
#include "AudioOffset.h"
#include "CliCommands.h"
#include "ColorCdl.h"
#include "CutWords.h"
#include "EditMotif.h"
#include "Export.h"
#include "McpToolProfiles.h"
#include "MediaTaskManager.h"
#include "SpeechOnset.h"
#include "WhisperDecodingProfile.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Document;
class EditLog;
class ProjectEditLog;
class Project;
class ProjectSessionLock;

// B2 -- ROADMAP.md. Every headless surface uses this serializer for a
// refusal, including commands implemented outside Cli.cc. Keeping the exit
// status beside the JSON construction prevents a caller from receiving a
// non-zero status with a success-shaped or free-form payload.
int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus = 1);
// BENCH-2026-09 (B3) -- the same refusal with a machine-readable `result`
// object, the shape McpToolRegistry::Call gives a dispatcher's partial
// result, so a refusal that names what to do reads the same on both
// surfaces. An empty or non-object `resultJson` is left out.
int FailCliCommandWithResult(const std::string& errorCode,
                             const std::string& detail,
                             const std::string& resultJson, std::string& output,
                             int exitStatus = 1);

// The reader of that envelope, for a backend that runs a command and reports
// through McpBackend: on success `resultJson` is the whole output; on failure
// `errorName` and `message` are its `error` and `detail`, and `resultJson`
// its `result` object when the refusal carries one (PRD20). CHAT-PARITE --
// the project and live backends share it, so a refusal reads the same from
// the MCP server and from the chat panel.
bool ReadCliCommandResult(const std::string& output, bool commandSucceeded,
                          std::string& resultJson, std::string& errorName,
                          std::string& message);
// PRD20 -- ROADMAP.md. A refusal of the command line itself (ValidationFailed
// with exit status 2) says how the command is called: `usage`, a
// CliUsageLine (CliCommands.h), is appended to its detail unless the detail
// already holds one. Any other output is returned as it is. For the commands
// that read their own arguments outside main() -- motion, multicam -- whose
// refusals main() relays without having written them.
std::string WithCliUsage(const std::string& output, int exitStatus,
                         const std::string& usage);
// PRD39 -- ROADMAP.md. `--help` promises that every command answers
// {"ok":true,...} or {"ok":false,...}, and --describe,
// --shot-quality-report, --isolated-frames and --locate-source-frame
// printed their report bare. This puts "ok":true in front of a JSON object
// that does not start with "ok", and changes nothing else: neither a byte
// of the report nor an answer that already carries "ok", or is no object.
std::string WithCliOk(const std::string& output);

// ADOBE-CLI-2026-09 -- the CLI and MCP must expose the same operation catalog
// so automation can discover and invoke capabilities without an HTTP server.
// PROFIL-2026-11 PF1 -- `profile`: what a server started with it lists.
int ListToolsCommand(std::string& output,
                     const McpToolProfile& profile = FullMcpToolProfile());
// PRD2 -- docs/roadmap/PRODUIT-2026-10.md. `--check-ffmpeg`: which FFmpeg
// this machine runs and what it lacks of what the engine uses, without a
// project -- the question a new install asks before it has one. DEC32 --
// exit status 0 as soon as a binary is found, as for `--check-setup`, with
// `complete` and, when it is false, `error`, `detail` and `missing` in the
// answer; 1, FfmpegNotFound, only when there is no binary.
int CheckFfmpegCommand(std::string& output);
// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. `--check-setup`: what this
// machine lacks, item by item (SetupCheck.h) -- FFmpeg, the Whisper model,
// MFA, the agent's key, the optional connections -- without a project:
// {"ok":…,"items":[…]}, and the missing required item's `error` and
// `detail` beside them when there is one. Exit status 0 when no required
// item is missing; FFmpeg is the only one. `--check-ffmpeg` exits the same
// way since DEC32.
int CheckSetupCommand(std::string& output);
// PRD19 -- ROADMAP.md. `--version`: what this binary is (BuildInfo.h), as
// {"ok":true,"version":"0.1.0","commit":"<short hash>","dirty":false}. No
// project, and nothing is read or started: the values are compiled in.
int VersionCommand(std::string& output);
// PRD18 -- ROADMAP.md (ACP4). `--procedure [<name> [<file>]]`: the editing
// procedures this binary carries (Procedures.h), in the shape get_procedure
// answers with and `ok` in front: the list, a procedure's SKILL.md, or one
// of its files. No project, nothing read from disk. `arguments` follow the
// command's name; a refusal of their shape has exit status 2, a name or a
// file that is not there 1.
int ProcedureCommand(const std::vector<std::string>& arguments,
                     std::string& output);

// RM3 -- docs/roadmap/REGLES-2026-10.md. `--train-caption-breaks <corpus>...
// [--evaluate <corpus>]... [--install] [--folds <n>] [--epochs <n>]
// [--step <n>]`: the forge of
// train_caption_breaks (RunCaptionBreakForge), its answer with `ok` in
// front. No project. A refusal of the command line has exit status 2; a
// corpus that does not read or does not suffice, 1.
int TrainCaptionBreaksCommand(const std::vector<std::string>& arguments,
                              std::string& output);

// DB1 -- docs/roadmap/DONNEES-2026-10.md. The same archive tools as MCP.
int ProjectArchiveCommand(const std::vector<std::string>& arguments, bool query,
                          std::string& output);
int ProjectArchiveMaintenanceCommand(const std::vector<std::string>& arguments,
                                     const std::string& tool,
                                     std::string& output);
// DB3, OR2 -- the same durable run step boundary as the MCP surface.
int RunStepCommand(const std::vector<std::string>& arguments, bool commit,
                   std::string& output);
// IA7 -- ROADMAP.md. `--ingest` under the project's session lock, refused
// while a server or the app holds it. IngestCommand (Ingest.h) stays the
// unlocked core that a lock holder runs.
// IA5 -- ROADMAP.md. Every other command that writes the package or one of
// its analysis caches, under the same lock: `arguments` is argv without the
// program name. `--ingest` and `--call-tool` take their own; `--create-project`
// writes a package nobody holds yet. A project path that does not exist is
// left to the command to refuse in its own words.
bool CliCommandWritesProject(const std::vector<std::string>& arguments);
// A package folder stands for the project file inside it, as it does for
// --call-tool and open_project: every other command derives its journals,
// its lock and its package from the file, and refused the folder as an
// "unsupported project package" -- a rule the interview skill had to spell
// out after it cost a run. Returns the project file when `path` is a
// .cutmachine-project package (project.cutmachine.json and manifest.json
// both inside), `path` unchanged otherwise: a file, a missing path or any
// other folder is left to the command to judge.
std::string ProjectPathOfPackage(const std::string& path);
bool LockProjectForCliWriter(const std::string& projectPath,
                             ProjectSessionLock& lock, std::string& output);
// DEC32 (PRD7) -- `targetBin` as IngestCommand takes it (Ingest.h).
int LockedIngestCommand(const std::string& projectPath,
                        const std::string& mediaPath, bool recursive,
                        std::string& output,
                        const std::optional<Ulid>& targetBin = std::nullopt);
int CallToolCommand(const std::string& projectPath, const std::string& toolName,
                    const std::string& argumentsJson, std::string& output,
                    bool requireExplicitTimeline = false);

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
// ITW7 -- ROADMAP.md. The first timeline used to be 1920x1080 at 25 fps
// whatever was shot, so a vertical interview began by deleting it. `format`,
// "WIDTHxHEIGHT@RATE" with RATE such as "25" or "24000/1001", sets it
// instead; empty keeps that default. The result names the timeline.
int CreateProjectCommand(const std::string& packagePath,
                         const std::string& projectName, std::string& output,
                         const std::string& format = "");
// ALPHA-2026-08 -- automation must be able to produce the same local
// transcript cache the AppKit action consumes; otherwise an agent can read a
// transcript but cannot complete the workflow that creates it.
// An empty `whisperModelPath` resolves the model configured locally
// (Transcription.h's ResolveConfiguredWhisperModel), which is how every
// caller that is not a human typing a path reaches this.
// QC-2026-09 A3 -- takes several media because the Whisper model load costs
// about 8 s and used to be paid once per call. Media whose measured audio
// level (Document.h, recorded at ingest) says they are mute are skipped and
// reported rather than sent to the model; `includeSilent` overrides that.
//
// CHAT-PARITE -- F2.4. `runner` is where the Whisper work runs (see
// MediaTaskManager.h): empty for the CLI and the MCP server, the editor's
// own task manager for the chat panel. Everything else -- which media, which
// model, the speech assessment, the result -- stays here, for all three.
//
// PERF5 -- ROADMAP.md. `stored`, here and on the analyses below, is the
// project as stored when the caller already holds it under the session lock
// (the MCP session's cache); the command parses the package when it is null.
// An analysis writes caches beside the package, never the package, so it
// takes the project const.
//
// PRD40 -- ROADMAP.md. A new transcription replaces the cached one unless it
// looks worse (CachedTranscriptPreferredReason): then the cache stays and
// the result says so (`cache_kept`). `overwrite` installs it regardless.
int TranscribeMediaCommand(
    const std::string& projectPath, const std::vector<std::string>& mediaIds,
    const std::string& whisperModelPath, const std::string& language,
    bool verbatim, bool includeSilent, std::string& output,
    const MediaTaskRunner& runner = {}, const Project* stored = nullptr,
    bool overwrite = false,
    WhisperDecodingProfile profile = WhisperDecodingProfile::Default);
// B7 -- ROADMAP.md. Transcribes the audio arrangement a timeline actually
// plays. The cache belongs to the project but is keyed by exact audible clip
// bounds, model and language, never by a temporary exported media file.
// `runner` as for TranscribeMediaCommand.
int TranscribeTimelineCommand(
    const std::string& projectPath, const std::string& timelineId,
    const std::string& language, bool verbatim, std::string& output,
    const MediaTaskRunner& runner = {}, const Project* stored = nullptr,
    WhisperDecodingProfile profile = WhisperDecodingProfile::Default);
// B7 -- transcribe a delivered media file directly into SRT. This is the same
// local decode/Whisper path as timeline transcription, without constructing a
// disposable project merely to make a file addressable as LibraryMedia.
int SrtFromMediaCommand(
    const std::string& mediaPath, const std::string& outputPath,
    const std::string& language, bool verbatim, std::string& output,
    WhisperDecodingProfile profile = WhisperDecodingProfile::Default);
// ALPHA-2026-08 -- the reason these take word indices and never a timecode
// is PHILOSOPHY.md principle 7: the caller (agent or human) names *which
// words*, CUTMACHINE resolves *which frames*. A caller that could pass a
// time would be a caller that could invent one.
// QC-2026-08 -- picture quality is measured, never judged, so the analysis
// has to be reachable without the app: an agent that can read a grade but
// cannot produce one has half a workflow. Mirrors TranscribeMediaCommand.
// IA5 -- `runner` as for TranscribeMediaCommand, so a background MCP task
// can cancel the decode; the same holds for the two analyses below.
int AnalyzeShotQualityCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output,
                              const MediaTaskRunner& runner = {},
                              const Project* stored = nullptr);
// F1.6 -- produce the same cached source-domain audio onsets that CLI, MCP
// and the live agent read. This is read-only document analysis, not an edit.
int AnalyzeAudioBeatsCommand(const std::string& projectPath,
                             const std::string& mediaId, std::string& output,
                             const MediaTaskRunner& runner = {},
                             const Project* stored = nullptr);
// PA3 -- docs/roadmap/PARITE-2026-11.md. Who speaks when in one rush: its
// sound made 16 kHz mono, read by the speaker embedding model through
// scripts/diarize.py (SpeakerDiarization.h), and the answer cached at
// `.cutmachine/speakers/<media id>.json`. `speakers` is how many voices the
// rush holds, which the caller knows and the model does not. Read-only
// analysis, not an edit.
int AnalyzeSpeakersCommand(const std::string& projectPath,
                           const std::string& mediaId, int32_t speakers,
                           std::string& output,
                           const MediaTaskRunner& runner = {},
                           const Project* stored = nullptr);
int ShotQualityReportCommand(const std::string& projectPath,
                             std::string& output);
// R8 -- docs/roadmap/REEL-2026-09.md. The frames a shot holds that its
// neighbours do not, over the source ranges the active timeline reads,
// transition handles included. `analyze` decodes and caches the ranges no
// cache covers yet; without it the command only reports what is already
// measured, and names what is not. Read-only: it edits nothing.
// PL3 -- `timelineId` reads another timeline without activating it, as
// --timeline-stats does: the ranges are a property of the timeline, so a
// caller asking about a variant must not have to switch to it first.
// `mediaIds`, when not empty, scans those media whole instead of the ranges
// a timeline reads (PlanMediaIsolatedFrameScan): the derush of a shoot.
// Events are still situated on the timeline wherever it plays them.
// The report is the MCP tools' answer as it stands; `--isolated-frames`
// puts the command line's "ok" in front of it (WithCliOk, PRD39).
int IsolatedFramesCommand(const std::string& projectPath,
                          const std::string& timelineId, bool analyze,
                          std::string& output,
                          const MediaTaskRunner& runner = {},
                          const std::vector<Ulid>& mediaIds = {},
                          const Project* stored = nullptr);
// QC-2026-09 (A8) -- read-only arithmetic over the active composited
// timeline. The command publishes exact time and ratio values so callers do
// not have to count clips or convert frame rates themselves.
// PL3 -- `timelineId` reads another timeline without activating it.
int TimelineStatsCommand(const std::string& projectPath, std::string& output,
                         const std::string& timelineId = {});
// OR4 -- docs/roadmap/DONNEES-2026-10.md. check_timeline at the command
// line: the same policy, the same verdict, the same report, with "ok" set to
// the verdict and the exit status 1 when a check fails. `policyJson` and
// `filePath` empty: the default policy, no file.
int CheckTimelineCommand(const std::string& projectPath,
                         const std::string& timelineId,
                         const std::string& policyJson,
                         const std::string& filePath, std::string& output);
// The derush sheet (BuildMediaSheetPlan): frames of the named rushes, no
// timeline needed, written and described like a contact sheet.
int MediaSheetCommand(const std::string& projectPath,
                      const std::string& outputPath,
                      const std::vector<Ulid>& mediaIds, int32_t maximumImages,
                      std::string& output);
// QC-2026-09 (A6) -- render the same deterministic timeline sampling exposed
// to MCP. The JPEG is written to outputPath and exact cell metadata is
// returned in the ordinary CLI envelope.
// DEC29 -- docs/roadmap/GRADE-2026-09.md. `graded`, --graded: each cell
// with its clip's grade and the sequence look (TimelineSheetSettings).
int ContactSheetCommand(const std::string& projectPath,
                        const std::string& outputPath,
                        const std::string& timelineId, int32_t maximumImages,
                        std::string& output, bool graded = false);
int CutSheetCommand(const std::string& projectPath,
                    const std::string& outputPath,
                    const std::string& timelineId, int32_t maximumImages,
                    std::string& output, bool graded = false);
// RC1 -- docs/roadmap/RACCORDS-2026-10.md. review_cuts from the command
// line: every junction of the timeline, or those at `frames`, rendered,
// measured and judged against the declared thresholds (CutReview.h); the
// bands written as one JPEG at `outputPath` when there are any.
int ReviewCutsCommand(const std::string& projectPath,
                      const std::string& outputPath,
                      const std::string& timelineId,
                      const std::vector<int64_t>& frames, int32_t maximumBands,
                      std::string& output);
// G1 -- docs/roadmap/GRADE-2026-09.md. measure_color from the command line:
// the same measure and the same reply (ColorMeasure.h), in the CLI envelope.
// `region` is "x,y,width,height", each an exact "num/den" or an integer;
// empty measures the whole picture.
int MeasureColorCommand(const std::string& projectPath,
                        const std::string& timelineId,
                        const std::vector<Ulid>& clipIds,
                        const std::string& position, const std::string& region,
                        std::string& output);
// G2 -- match_color from the command line: the same match, the same reply
// (ColorMeasure.h). `match` is a comma-separated subset of exposure,
// balance and saturation; empty is all three. Changes nothing.
int MatchColorCommand(const std::string& projectPath,
                      const std::string& timelineId, const Ulid& clipId,
                      const Ulid& referenceClipId, const std::string& position,
                      const std::string& referencePosition,
                      const std::string& region,
                      const std::string& referenceRegion,
                      const std::string& match, std::string& output);
// G4 -- export_cdl from the command line: the same fold, the same reply
// (ColorCdl.h); `file`, when given, is written as .cc, .ccc or .cdl.
int ExportCdlCommand(const std::string& projectPath,
                     const std::string& timelineId,
                     const std::vector<Ulid>& clipIds, const std::string& file,
                     int resolveNode, std::string& output);
// G4 -- apply_cdl from the command line, applied through the path
// --apply-op takes, one undo step. The CDL comes from `file` (and `id` when
// it holds several) or from `resolve`, Resolve's strings; `space` is the
// values it was made on. `preview` prints the operation instead.
int ApplyCdlCommand(const std::string& projectPath,
                    const std::string& timelineId,
                    const std::vector<Ulid>& clipIds, const std::string& space,
                    const std::string& file, const std::string& id,
                    const ResolveCdlMap& resolve, bool preview,
                    std::string& output);
// ONSET-2026-08 -- same shape, and for the same reason: where the voice
// starts is a measurement, so it has to be reachable without the app. The
// report publishes the trim in whole frames precisely so a caller never
// computes one (PHILOSOPHY.md principle 7).
int AnalyzeSpeechOnsetCommand(const std::string& projectPath,
                              const std::string& mediaId, std::string& output,
                              const SpeechOnsetSettings& settings = {},
                              const MediaTaskRunner& runner = {},
                              const Project* stored = nullptr);
// SYNCHRO-2026-10 SY1 -- how far `otherId`'s sound sits from
// `referenceId`'s, exact to the sample at 48 kHz, or refused by name
// (AudioOffset.h). Read only.
int MeasureAudioOffsetCommand(const std::string& projectPath,
                              const std::string& referenceId,
                              const std::string& otherId,
                              const AudioOffsetChannels& channels,
                              std::string& output,
                              const MediaTaskRunner& runner = {},
                              const Project* stored = nullptr);
// FC6 -- FILMCRAFT-2026-10. Where one rush's voice speaks, read on the
// channel asked, and the gain curve a music under it would follow
// (AudioDspDucking.h), at 48 kHz. Read only: nothing is cached, nothing is
// written, the document is not touched (the clip gain cannot be animated
// yet).
int ProposeDuckingCommand(const std::string& projectPath,
                          const std::string& mediaId,
                          AudioOffsetChannel channel,
                          const audio_dsp::DuckingSettings& settings,
                          std::string& output,
                          const MediaTaskRunner& runner = {},
                          const Project* stored = nullptr);
// SYNCHRO-2026-10 SY2 -- a recorder's sound laid under a picture clip of
// the active timeline, linked, in one undo (SeparateSound.h). Without an
// offset, it is measured first, as --measure-audio-offset measures it.
struct SyncAudioOptions {
    AudioOffsetChannels channels;
    bool has_offset = false;
    RationalTime offset{0, 1};
    bool mute_camera_sound = true;
};
int SyncAudioCommand(const std::string& projectPath, const std::string& clipId,
                     const std::string& mediaId, const std::string& trackId,
                     const SyncAudioOptions& options, std::string& output);
// SY2, the window -- the recorder's sound already on the timeline: slid into
// sync with the picture on its own track and linked, in one undo
// (BuildSeparateSoundAlign), as the window's Auto Align does.
int SyncAudioClipCommand(const std::string& projectPath,
                         const std::string& clipId,
                         const std::string& soundClipId,
                         const SyncAudioOptions& options, std::string& output);
int SpeechOnsetReportCommand(const std::string& projectPath,
                             std::string& output,
                             const SpeechOnsetThresholds& thresholds = {});
// ALIGN-2026-08 -- reports which cached word boundaries the speech envelope
// contradicts, and where each one belongs. Read-only unless `apply`: a
// transcript is a cache artifact several tools read, and rewriting it under
// them is a decision for the caller, not for a report.
//
// QC-2026-09 (A1) -- `apply` is that decision. Without it the correction
// exists only in a report, and every tool that cuts on words keeps cutting on
// the boundaries the signal contradicts; the measured cost of that was some
// forty probe round trips in one session. The document is never touched
// either way -- this rewrites a cache file, which a re-transcription
// regenerates.
int AlignTranscriptsCommand(const std::string& projectPath, bool apply,
                            std::string& output,
                            const Project* stored = nullptr);
// AX2 -- docs/roadmap/AX-2026-09.md. MFA's word boundaries for the rushes
// named, or for every rush Whisper has read when none is: scripts/
// mfa_align.py in MFA's environment (MfaAlignment.h), as a media task
// through `runner`, writing the alignment cache every word-level tool reads.
// `textsJson`, empty or {media id: text}, replaces a rush's Whisper text
// where speech has no word (the answer's uncovered_speech names those). The
// answer says, per rush, how many words the engine now reads and who timed
// them (word_timing), what was skipped and why.
int AlignWordsCommand(const std::string& projectPath,
                      const std::vector<std::string>& mediaIds,
                      const std::string& textsJson, std::string& output,
                      const MediaTaskRunner& runner = {},
                      const Project* stored = nullptr);
int ListDisfluenciesCommand(const std::string& projectPath,
                            const std::string& clipId, std::string& output);
int RemoveWordsCommand(const std::string& projectPath,
                       const std::string& clipId, const std::string& rangesJson,
                       std::string& output);
// QC-2026-09 A2 -- closes the silences inside one clip, from the speech
// envelope alone. Applies one reversible RemoveWordsOperation carrying the
// clip's A/V pair, and reports which pauses it closed. Needs a cached
// speech onset analysis for the clip's source; it needs no transcript,
// which is the point -- tightening a take should not wait on Whisper.
int TightenPausesCommand(const std::string& projectPath,
                         const std::string& clipId, int64_t minimumGapMs,
                         int64_t keepFrames, std::string& output);
// B4 -- ROADMAP.md. Closes measured head/tail air without consulting word
// timestamps. The junction form measures both sides before applying one
// atomic operation, so it cannot oscillate over successive passes.
int TrimBoundaryAirCommand(const std::string& projectPath,
                           const std::string& clipId, int64_t keepFrames,
                           int64_t minimumAirMs, std::string& output);
int CloseJunctionAirCommand(const std::string& projectPath,
                            const std::string& leftClipId,
                            const std::string& rightClipId, int64_t keepFrames,
                            int64_t minimumAirMs, std::string& output);
// QC-2026-09 A4 -- read-only. Reports which clips of the active timeline play
// a given frame of a given rush, and where. The conversion it performs is the
// one every hand-computed timeline position in a measured session got wrong
// at least once.
int LocateSourceFrameCommand(const std::string& projectPath,
                             const std::string& mediaId, int64_t sourceFrame,
                             std::string& output);
// PL3 -- `timelineId` describes another timeline without activating it;
// empty, the active one.
int DescribeCommand(const std::string& documentPath, std::string& output,
                    const std::string& timelineId = {});
// SRT-2026-08 -- read-only. Subtitles were reachable only from the app: the
// cue builder and SaveSrt both had their single caller in main.mm, so a
// headless montage could be transcribed but never subtitled, against
// AGENTS.md's rule that what only the mouse can reach does not exist. Cues
// come from the audible audio tracks, because subtitles follow what is
// heard, not what is on screen -- an overlay laid over someone else's words
// must not caption itself.
int ExportSrtCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output,
                     const std::string& timelineId = {},
                     bool wordByWord = false);
// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The same cues and report as
// WebVTT (WriteVtt), `format: "webvtt"` added to say which was written.
int ExportVttCommand(const std::string& projectPath,
                     const std::string& outputPath, std::string& output,
                     const std::string& timelineId = {},
                     bool wordByWord = false);
// SEQ-2026-08 -- read-only: reports the sequence format the project's rushes
// imply, without touching the document. Conforming to it is a separate,
// journalized UpdateSequenceOperation.
int ProposeSequenceCommand(const std::string& projectPath, std::string& output);

// The same JSON view DescribeCommand produces (sequence/tracks/library/bins/
// markers, with the A1/A2.../K1... aliases the MCP tool catalog's ID
// resolver and the chat panel both key off), computed directly from an
// in-memory Document instead of a project file path. See McpLiveBackend.h.
std::string DescribeDocument(const Document& document);
std::string DescribeProject(const Project& project,
                            const std::string& timelineId = {});
// IA9 -- ROADMAP.md. When the edit takes sound out of the timeline, the
// result carries `cut_words` (CutWords.h): the aligned words it removed or
// cut into, split between those `targetedWords` names and those carried off.
//
// MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. `motifs` ride on the journal
// entry the edit writes (EditLog::Apply).
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson, std::string& output,
                          const std::string& timelineId = {},
                          const TargetedWords& targetedWords = {},
                          const std::vector<EntryMotif>& motifs = {});
// PERF5 -- ROADMAP.md. The same edit on a project the caller already holds
// (the MCP session's cache, under its session lock): `project` is the stored
// state on entry and, on success, the state just committed, so the caller
// keeps it instead of parsing the package back. On failure it is left in an
// unspecified state and must be dropped.
int ApplyOperationToLoadedProject(const std::string& documentPath,
                                  Project& project,
                                  const std::string& operationJson,
                                  std::string& output,
                                  const std::string& timelineId = {},
                                  const TargetedWords& targetedWords = {},
                                  const std::vector<EntryMotif>& motifs = {});
// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. `motifs` ride on the project
// journal's entry (ProjectEditLog::Apply), a batch's included.
int ApplyProjectOperationCommand(const std::string& projectPath,
                                 const std::string& operationJson,
                                 std::string& output,
                                 const std::vector<EntryMotif>& motifs = {});
// PERF5 -- ROADMAP.md. The project edit on a project the caller already
// holds, with ApplyOperationToLoadedProject's contract: `project` is the
// stored state on entry and the committed one on success, and must be
// dropped on failure.
int ApplyProjectOperationToLoadedProject(
    const std::string& projectPath, Project& project,
    const std::string& operationJson, std::string& output,
    const std::vector<EntryMotif>& motifs = {});
int UndoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);
int RedoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);

// Timeline-level undo/redo counterparts to ApplyOperationCommand. Operate on
// the active timeline's own EditLog, the same log --apply-op appends to, so
// an MCP undo/redo tool call is indistinguishable on disk from a human
// triggering undo/redo in the app. There is no CLI flag for these yet; they
// exist so the MCP server (Operations.h ticket F1.1) can reuse this exact
// path instead of duplicating EditLog::Undo/Redo call sites.
int UndoOperationCommand(const std::string& documentPath, std::string& output,
                         const std::string& timelineId = {});
int RedoOperationCommand(const std::string& documentPath, std::string& output,
                         const std::string& timelineId = {});
// PERF5 -- ROADMAP.md. Undo and redo on a project the caller already holds,
// with ApplyOperationToLoadedProject's contract.
int UndoOperationOnLoadedProject(const std::string& documentPath,
                                 Project& project, std::string& output,
                                 const std::string& timelineId = {});
int RedoOperationOnLoadedProject(const std::string& documentPath,
                                 Project& project, std::string& output,
                                 const std::string& timelineId = {});
// PL1 -- ROADMAP.md. An agent's intervention as one undo step, for a
// client whose intervention spans several tool calls. Begin reads the
// timeline's journal and writes nothing: its token names the timeline, the
// entries applied so far and the state they lead to. End folds what the
// journal gained since into one labelled entry (EditLog::GroupSince) and
// leaves the document untouched. No CLI flag either, like undo/redo:
// `--call-tool begin_intervention` and `end_intervention` reach them.
int BeginInterventionCommand(const std::string& projectPath,
                             std::string& output,
                             const std::string& timelineId = {});
// MOTIF2 -- `motif` covers the folded members no motif covers yet.
int EndInterventionCommand(
    const std::string& projectPath, const std::string& intervention,
    const std::string& label, std::string& output,
    const std::optional<EditMotif>& motif = std::nullopt);
// PL3 -- ROADMAP.md. Edit plans from the CLI. A plan lives in the session
// that prepared it, and each CLI invocation is a session of its own, so the
// plan is kept in a file its caller names (EditPlan.h's format): outside the
// project, never read by anything but these commands. Prepare runs the same
// prepare_edit_plan the MCP catalog does, with the same arguments, and
// refuses to overwrite an existing file; apply runs apply_edit_plan on the
// plan read back, refused as stale like any other; discard removes a file
// only once it has been read as a plan.
int PrepareEditPlanCommand(const std::string& projectPath,
                           const std::string& argumentsJson,
                           const std::string& planPath, std::string& output,
                           bool requireExplicitTimeline = false);
int ApplyEditPlanCommand(const std::string& projectPath,
                         const std::string& planPath, std::string& output,
                         bool requireExplicitTimeline = false);
int DiscardEditPlanCommand(const std::string& planPath, std::string& output);
int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output,
                  const std::string& timelineId = {});
// PRD16 -- ROADMAP.md. `--export` as it was typed, before the project is
// read. A codec named alone (--codec h264|hevc) keeps the settings it has
// always made; a preset key (--preset, Exporter::Presets) is resolved at the
// format of the timeline exported; neither is the engine's default for that
// timeline (Exporter::DefaultSettings) -- no longer the CLI's own choice, so
// an SDR timeline is now written in H.264 High and `--codec hevc` gives the
// file it wrote before. A preset beside a codec it does not encode, and a
// key no preset has, are refused by name.
struct ExportCommandChoice {
    std::string codec;
    std::string preset;
    bool software = false;
    bool overwrite = false;
    bool rec709a = false;
    // PA2 -- `--loudness <LUFS>`: the delivery's loudness target.
    ExportLoudnessTarget loudness;
};
int ExportCommand(const std::string& documentPath,
                  const std::string& outputPath,
                  const ExportCommandChoice& choice,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output,
                  const std::string& timelineId = {});

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId);
std::string ProjectEditLogPathForProject(const std::string& projectPath);
// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The project's journal as stored
// in its package, for list_motifs to read; a project never edited at the
// project level has none, and `log` is left empty.
bool LoadProjectEditLogIfAny(const std::string& projectPath,
                             ProjectEditLog& log, EditError& error,
                             std::string& message);

// Transactional primitive for storage layouts that add package-local
// artifacts. All destinations must be regular files on the same filesystem.
bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message);

// S2 -- SAVING_ROADMAP.md. Recovers an interrupted transaction rooted in
// directory and removes leftovers whose durable commit marker is already gone.
bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message);

// S4 -- SAVING_ROADMAP.md. Project packages use this variant so removal of
// obsolete canonical files belongs to the generation being published.
bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message);
