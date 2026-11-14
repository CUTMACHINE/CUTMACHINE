#pragma once

// GENERATIF-2026-10 -- docs/roadmap/GENERATIF-2026-10.md. Media the engine
// asks a paid service to make: at first one kind, the extension of a picture
// to another aspect ratio (expand_image, GN4), on fal.ai with the person's
// own key.
//
// Generation costs money on the person's account. The author's rule, written on
// 21 October 2026: nothing is generated unless the person asked for it; an
// agent may propose a generation, and spends only once the person has
// agreed. The engine holds that rule in layers instead of trusting a model
// to remember it (PHILOSOPHY.md, principe 7):
//
//   - a proposal spends nothing: it says what would be sent, to which
//     model, at what announced cost, and gets an id (GenerationProposal);
//   - a generation needs that id and an explicit confirmation, spends the
//     proposal once and only within its lifetime (GenerationProposalStore);
//   - the tools say what they are (McpToolClass::External for the one that
//     spends), and a client that asks the person before an external call
//     asks here: the Agent panel no longer approves this one on its own
//     (ToolSpendsOnPersonsAccount, AcpClient.cc).
//
// What none of these layers can do is tell the person's "yes" from the
// agent's: an agent that writes `confirmed_by_person: true` without asking
// has lied, and only a client that shows the call to the person before it
// runs stops it. The limit is said in the reference (generation.md).
//
// Plain C++: the HTTPS request and the secret store are a seam the
// executable installs (GenerationTransport.mm on macOS: NSURLSession and the
// Keychain). A process without one -- the build without a window, a test
// that installs none -- refuses by name.

#include "Document.h"
#include "Json.h"
#include "Ulid.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace generation {

// ---------------------------------------------------------------------
// GN1 -- the transport seam and the key.
// ---------------------------------------------------------------------

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // How long the transport may wait for the whole answer; 0 leaves it to
    // the transport.
    double timeout_seconds = 0.0;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string content_type;
};

// True once an HTTP response was received, whatever its status; false only
// when none was (DNS, TLS, timeout), with `error` saying so in words that
// carry no secret.
using HttpTransport =
    std::function<bool(const HttpRequest&, HttpResponse&, std::string& error)>;

// What the executable installs. A plain function pointer, as the other
// seams of the model are (FaceFinder.h): the slot outlives any closure.
using NativeHttp = bool (*)(const HttpRequest&, HttpResponse&, std::string&);
// Reads the key from the machine's secret store. False with `message` when
// there is none; `message` never holds the key.
using NativeKeyReader = bool (*)(std::string& key, std::string& message);

void SetGenerationTransport(NativeHttp transport);
NativeHttp CurrentGenerationTransport();
void SetGenerationKeyReader(NativeKeyReader reader);
NativeKeyReader CurrentGenerationKeyReader();

// Installed by the macOS executable (GenerationTransport.mm).
void InstallGenerationTransport();

// The refusal of a process without a transport.
inline constexpr char kNoTransportMessage[] =
    "aucun transport HTTPS pour la génération dans cet exécutable : elle "
    "passe par l'application macOS (NSURLSession). La version sans "
    "interface ne génère rien";

// The variable the fal.ai clients read; the fallback when the secret store
// holds nothing.
inline constexpr char kFalKeyVariable[] = "FAL_KEY";
// Where the macOS executable reads the key (GenerationTransport.mm): a
// generic password the person stores once with `security
// add-generic-password -s <service> -a <account> -w`.
inline constexpr char kFalKeychainService[] = "com.cutmachine.generation";
inline constexpr char kFalKeychainAccount[] = "fal-api-key";

// The key, from the secret store first and FAL_KEY second, with where it
// was found ("trousseau" or "FAL_KEY"). False with a message saying how to
// set one when neither holds it. The key is never written to a file, a log
// or an answer by anything in this file.
bool ResolveFalKey(std::string& key, std::string& origin, std::string& message);

// `text` with every occurrence of `secret` replaced: what an error from the
// service is passed through before anyone sees it. A secret shorter than 8
// characters is left alone -- it would blank ordinary words.
std::string RedactSecret(std::string text, const std::string& secret);

// ---------------------------------------------------------------------
// GN4 -- what can be generated.
// ---------------------------------------------------------------------

struct GenerationKind {
    // The engine's name, the tool's `kind`.
    const char* kind;
    const char* service;
    // The service's endpoint id.
    const char* model;
    // What comes back: "image" for now.
    const char* output;
    // What the request and the answer were checked against, and what was
    // not checked: said to the person in every proposal.
    const char* verified;
    // The announced cost, "" when the engine does not know it. Never a
    // guess: fal.ai publishes its prices on each model's page, which was
    // not reachable when this was written.
    const char* announced_cost;
    // PA12 -- the kind changes what a FILMED person says or looks like (a
    // voice put in another language, lips moved to another sound): it runs
    // only with that person's consent, said in the call (DEC51).
    bool acts_on_filmed_person = false;
};

// PA12 -- docs/roadmap/PARITE-2026-11.md. The creative kinds, beside
// expand_image: a shot or a picture made from words (stock_video,
// stock_image), a synthetic presenter speaking a sound (avatar), a filmed
// person's lips moved to another sound (lipsync), a film's voices put in
// another language (dubbing). Each is a fal.ai model whose request and
// answer were read in its public documentation on 11 November 2026 (the
// model page's llms.txt and OpenAPI), never called: GenerationKind::verified
// says so in every proposal.
//
// DEC51 -- the author, 11 November 2026 ("reste créatif donc ça peut être
// dedans"), amending DEC2 of 23 October, which kept every generation off a
// filmed person. lipsync and dubbing act on one; they run only when the
// call carries `consent_of_filmed_person: true`, and an avatar made from a
// picture the engine did not generate needs it too. The engine cannot check
// a consent: like `confirmed_by_person`, it is a statement the caller makes
// and a client shows to the person before the call runs.
//
// No kind corrects a gaze: fal.ai offers no such model today (searched on
// 11 November 2026; `fal-ai/eye-correct` answers 404).
//
// PA14 -- one more, asked by the author on 13 November 2026: a shot edited by
// words and reference pictures, its motion kept (edit_video) -- a person or
// a thing replaced, a place or a look changed. fal.ai's Kling 4.0 edit
// (`kling/v4/video-to-video/edit`), read in the model's llms.txt that day.
// It acts on what was filmed: it needs the consent unless the shot and
// every reference picture were generated by the engine.
bool IsCreativeKind(const std::string& kind);

// PA14 -- what Kling 4.0 edit reads of a shot (its own stated bounds): a
// stretch of 3 to 30 seconds, no side under 700 pixels. The engine refuses
// outside them before anything is sent. The model does not publish how
// many reference pictures it takes; four is what its siblings (Kling O1,
// O3) state.
inline constexpr int64_t kEditVideoMinSeconds = 3;
inline constexpr int64_t kEditVideoMaxSeconds = 30;
inline constexpr int64_t kEditVideoMinSide = 700;
inline constexpr size_t kEditVideoMaxReferences = 4;

// PA14 -- what an edit of `seconds` (a whole number, rounded up) is billed
// at `resolution` ("720p", "1080p"), in thousandths of a dollar: the
// published rate times the input plus the output, the output lasting as
// long as the input. 0 for a resolution with no published rate.
int64_t EditVideoMilliDollars(int64_t seconds, const std::string& resolution);

// A file that goes up with a creative request.
struct GenerationUpload {
    // The request field that takes its URL: "video_url", "audio_url",
    // "image_url"; or "image_urls" (PA14), an array each such upload adds
    // its URL to, in order.
    std::string field;
    Ulid media_id;
    std::string path;
    // "still": the file as it is. "frame": one frame at `time`. "excerpt":
    // [time, time + duration) of the media, picture and sound. "sound":
    // the same range, its sound only.
    std::string what;
    RationalTime time{0, 1};
    RationalTime duration{0, 1};
};

// The longest stretch of a media a request sends, and the largest file:
// the storage's plain upload takes one request, and the models' own bounds
// (a dubbing of ten minutes, a lipsync without a stated one) are beyond
// what a film of this trade asks of them.
inline constexpr int64_t kMaxExcerptSeconds = 300;
inline constexpr size_t kMaxUploadBytes = 90u * 1024u * 1024u;

// [in, in + duration) of `sourcePath` as a file a model reads: H.264 and
// AAC in an .mp4, the picture no larger than 1920 on its long side, or --
// `soundOnly` -- 16-bit PCM at 48 kHz in a .wav. The picture goes as the
// file encodes it: a log rush stays log -- unless `colorFilter` is given
// (PA14): FFmpeg filters that turn the picture into a Rec.709 one
// (ColorLutFilter, ColorManagement.h), after which the file is tagged
// BT.709. Written to `outPath`, which must not exist. Through
// FfmpegBinary.h.
bool EncodeExcerpt(const std::string& sourcePath, const RationalTime& in,
                   const RationalTime& duration, bool soundOnly,
                   const std::string& outPath, std::string& message,
                   const std::string& colorFilter = std::string());

// The answer of a creative kind: the URL of what it made (`video.url`, or
// `images[0].url` for stock_image), and the seed when the model gives one.
bool ParseCreativeOutput(const std::string& kind, const std::string& json,
                         std::string& url, std::string& seed,
                         std::string& message);

const std::vector<GenerationKind>& GenerationKinds();
const GenerationKind* FindGenerationKind(const std::string& kind);

// GN4 -- where the picture goes in its extended canvas: integers, computed
// here so that no model works out a canvas (principe 7).
struct ExpandCanvas {
    int64_t canvas_width = 0;
    int64_t canvas_height = 0;
    int64_t image_width = 0;
    int64_t image_height = 0;
    // The upper left corner of the picture in the canvas.
    int64_t x = 0;
    int64_t y = 0;
};

// "9:16", "4:5", "1:1"... into two positive integers.
bool ParseAspectRatio(const std::string& text, int64_t& width, int64_t& height,
                      std::string& message);

// The smallest canvas of aspect `aspectWidth`:`aspectHeight` with even
// sides that holds the picture at its own size, the picture centred in it
// (the remainder of an odd margin goes right or below). Even, because the
// picture becomes an H.264 4:2:0 clip (EncodeStillClip). Refused when the
// picture already has that aspect, when the canvas would pass the model's
// 5000 x 5000 pixels, or when the picture would cover less than 15 % of it
// -- the two bounds the model's own description states
// (fal-ai/bria/expand).
bool ComputeExpandCanvas(int64_t imageWidth, int64_t imageHeight,
                         int64_t aspectWidth, int64_t aspectHeight,
                         ExpandCanvas& canvas, std::string& message);

// The request body of fal-ai/bria/expand, without its image_url: what a
// proposal shows and what generate_media sends, the uploaded URL added.
mcp_json::Value ExpandInput(const ExpandCanvas& canvas,
                            const std::string& prompt,
                            const std::string& negativePrompt,
                            const std::string& seed);

// GN3 -- a picture made into a clip the library takes. When this was
// written the library held no still (REEL-2026-09 R4, open then; ingest
// has taken a PNG as a one-frame media since 24 October 2026, DEC31), so
// the picture the service returned is kept as it came, beside a clip of
// `frames` frames at `rate` made from it: H.264
// 4:2:0, quality 12, tagged BT.709 as every video the engine reads, an odd
// side cropped by one pixel. Through FFmpegBinary.h, like every launch of
// FFmpeg. Written to `clipPath`, which must not exist.
bool EncodeStillClip(const std::string& picturePath,
                     const std::string& clipPath, const MediaRate& rate,
                     int64_t frames, std::string& message);

// The answer of fal-ai/bria/expand: the picture's URL, the seed it used,
// and its size when given (0 otherwise).
bool ParseExpandOutput(const std::string& json, std::string& imageUrl,
                       std::string& seed, int64_t& width, int64_t& height,
                       std::string& message);

// ---------------------------------------------------------------------
// GN1 -- fal.ai, through the queue.
// ---------------------------------------------------------------------

// The protocol as the official JavaScript client (@fal-ai/client 1.10.1,
// read from npm on 21 October 2026) speaks it: a POST to
// https://queue.fal.run/<endpoint> with `Authorization: Key <key>` answers
// a request_id; GET https://queue.fal.run/<owner>/<alias>/requests/<id>/
// status until "COMPLETED"; GET .../requests/<id> for the result; PUT
// .../requests/<id>/cancel to stop. A file goes up by POST
// https://rest.fal.ai/storage/upload/initiate?storage_type=fal-cdn-v3
// ({content_type, file_name} -> {upload_url, file_url}) and a PUT of its
// bytes to upload_url. The key goes to queue.fal.run and rest.fal.ai only:
// never to an upload URL, never to the URL of a result.
struct FalSettings {
    std::string queue_base = "https://queue.fal.run";
    std::string rest_base = "https://rest.fal.ai";
    double poll_interval_seconds = 1.0;
    // How long a generation may take before it is cancelled. Twenty
    // minutes since PA14: a video model edits thirty seconds of film, and a
    // request cancelled once it has started may still be billed.
    double max_wait_seconds = 1200.0;
    // Waits between two status reads. Empty: the thread sleeps.
    std::function<void(double seconds)> sleep;
};

class FalClient {
public:
    FalClient(HttpTransport transport, std::string key,
              FalSettings settings = {});

    // Uploads `bytes` to fal.ai's storage and gives the URL the model reads.
    bool Upload(const std::string& bytes, const std::string& contentType,
                const std::string& fileName, std::string& fileUrl,
                std::string& message) const;
    // Submits `input` to `endpoint`, waits for it, and gives the result's
    // JSON and the request id. A wait past max_wait_seconds cancels it.
    bool Run(const std::string& endpoint, const mcp_json::Value& input,
             std::string& resultJson, std::string& requestId,
             std::string& message) const;
    // Fetches a result file, without the key.
    bool Download(const std::string& url, std::string& bytes,
                  std::string& contentType, std::string& message) const;

private:
    bool Send(HttpRequest request, bool withKey, HttpResponse& response,
              std::string& message) const;

    HttpTransport transport_;
    std::string key_;
    FalSettings settings_;
};

// The queue's base path of an endpoint: "fal-ai/bria/expand" ->
// "fal-ai/bria" (owner and alias only, as the client builds it).
std::string FalQueueAppPath(const std::string& endpoint);

// ---------------------------------------------------------------------
// GN2 -- proposals: what is shown before anything is spent.
// ---------------------------------------------------------------------

struct GenerationProposal {
    Ulid id;
    std::string kind;
    std::string service;
    std::string model;
    std::string announced_cost;
    // The library media the picture comes from, the file read, and whether
    // a frame of it is taken at `source_time` (a video) or the file sent as
    // it is (a still).
    Ulid source_media_id;
    std::string source_path;
    bool source_is_frame = false;
    RationalTime source_time;
    int64_t source_width = 0;
    int64_t source_height = 0;
    std::string aspect_ratio;
    ExpandCanvas canvas;
    std::string prompt;
    std::string negative_prompt;
    std::string seed;
    // The clip the picture becomes (EncodeStillClip): the source's rate for
    // a frame of a video, 25/1 for a still.
    MediaRate clip_rate;
    int64_t clip_frames = 0;
    // UTC, ISO 8601: when it was proposed.
    std::string proposed_at;
    std::chrono::steady_clock::time_point created;
    // PA12 -- a creative kind: the request as it will be sent, less the
    // URLs of the files that go up with it (an object; null for
    // expand_image, which keeps the fields above), those files, and
    // whether the call needs the filmed person's consent (DEC51).
    mcp_json::Value input;
    std::vector<GenerationUpload> uploads;
    bool needs_consent = false;
};

// How long a proposal can be confirmed. Past it, the person is asked again
// with a fresh one: a "yes" given this morning is not a yes to whatever the
// proposal has become.
inline constexpr int kProposalLifetimeMinutes = 30;

// The proposals of one session (one server, one chat panel). Thread-safe.
class GenerationProposalStore {
public:
    void Add(const GenerationProposal& proposal);
    // The proposal `id` names, still valid at `now`, taken out of the
    // store: a proposal is spent once, whatever happens to the generation
    // after (a failure asks for a new proposal, never a silent retry). False
    // with `message` when it is unknown, already spent or expired.
    bool Take(const Ulid& id, std::chrono::steady_clock::time_point now,
              GenerationProposal& proposal, std::string& message);
    // The same checks as Take, leaving the proposal in the store: what a
    // generation reads before it spends anything.
    bool Peek(const Ulid& id, std::chrono::steady_clock::time_point now,
              GenerationProposal& proposal, std::string& message);
    size_t Size() const;

private:
    bool Find(const Ulid& id, std::chrono::steady_clock::time_point now,
              bool take, GenerationProposal& proposal, std::string& message);

    mutable std::mutex mutex_;
    std::map<Ulid, GenerationProposal> proposals_;
    std::map<Ulid, std::string> closed_;  // id -> why it is closed
};

// What a proposal says to the person and to the agent: the answer of
// propose_generation.
mcp_json::Value ProposalValue(const GenerationProposal& proposal,
                              bool keyConfigured, bool transportAvailable);

// The provenance of what a proposal produced.
MediaGeneration ProvenanceOf(const GenerationProposal& proposal,
                             const std::string& seed,
                             const std::string& requestId,
                             const std::string& createdAt);

// The current time, UTC, ISO 8601 to the second.
std::string UtcNow();

}  // namespace generation

// GN2 -- the tools of the catalog that spend money on the person's account:
// a client that approves the engine's tools on its own does not approve
// these (AcpClient.cc). Read by name, with no catalog, so that the Agent
// panel's client needs nothing else from the engine.
bool ToolSpendsOnPersonsAccount(const std::string& toolName);
