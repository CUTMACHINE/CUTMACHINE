// GENERATIF-2026-10 -- docs/roadmap/GENERATIF-2026-10.md. Paid generation
// against a fake fal.ai: no request leaves the process, and no key is real.
//
//   - the extension canvas (GN4): exact aspect, centred, the model's bounds;
//   - fal.ai's queue (GN1): who gets the key, the polling, the cancel, the
//     errors passed through without the key;
//   - proposals (GN2): spent once, expired after their lifetime;
//   - the provenance (GN3): an operation that round-trips byte for byte and
//     declares version 20;
//   - propose_generation then generate_media on a real project, through the
//     catalog: nothing spent without the person's confirmation, a key and a
//     transport; then a picture in the package, ingested, marked;
//   - PA14: a shot edited by words and reference pictures (edit_video).

#include "Document.h"
#include "EditLog.h"
#include "FfmpegBinary.h"
#include "Generation.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "OperationFormat.h"
#include "Operations.h"
#include "ProcessLauncher.h"
#include "Project.h"
#include "ProjectStorage.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using mcp_json::Value;
namespace fs = std::filesystem;

Value Parse(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        std::cerr << "unparsable test JSON: " << text << " (" << error << ")\n";
        std::abort();
    }
    return value;
}

std::string At(const Value& root, std::initializer_list<const char*> keys) {
    const Value* current = &root;
    for (const char* key : keys) {
        // PA12 -- a key of digits names an element of an array.
        if (current->IsArray()) {
            const size_t index = static_cast<size_t>(std::atoi(key));
            if (index >= current->AsArray().size()) return {};
            current = &current->AsArray()[index];
            continue;
        }
        current = current->Find(key);
        if (current == nullptr) return {};
    }
    return current->IsString() ? current->AsString() : current->Dump();
}

// ---------------------------------------------------------------------
// A PNG written by hand: stored deflate blocks, so no library is needed.
// ---------------------------------------------------------------------

uint32_t Crc32(const std::string& bytes) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string BigEndian(uint32_t value) {
    std::string out(4, '\0');
    for (int index = 0; index < 4; ++index)
        out[index] = static_cast<char>((value >> (24 - 8 * index)) & 0xff);
    return out;
}

std::string Chunk(const std::string& type, const std::string& data) {
    return BigEndian(static_cast<uint32_t>(data.size())) + type + data +
           BigEndian(Crc32(type + data));
}

std::string Png(int width, int height) {
    std::string raw;
    for (int y = 0; y < height; ++y) {
        raw += '\0';  // no filter
        for (int x = 0; x < width; ++x) {
            raw += static_cast<char>(x * 7);
            raw += static_cast<char>(y * 11);
            raw += static_cast<char>(128);
        }
    }
    // zlib: header, stored blocks of at most 65535 bytes, Adler-32.
    std::string zlib = "\x78\x01";
    for (size_t offset = 0; offset < raw.size(); offset += 65535) {
        const size_t length = std::min<size_t>(65535, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib += static_cast<char>(last ? 1 : 0);
        zlib += static_cast<char>(length & 0xff);
        zlib += static_cast<char>((length >> 8) & 0xff);
        zlib += static_cast<char>(~length & 0xff);
        zlib += static_cast<char>((~length >> 8) & 0xff);
        zlib += raw.substr(offset, length);
    }
    uint32_t a = 1;
    uint32_t b = 0;
    for (unsigned char byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    zlib += BigEndian((b << 16) | a);
    std::string header = BigEndian(static_cast<uint32_t>(width)) +
                         BigEndian(static_cast<uint32_t>(height));
    header += '\x08';  // 8 bits
    header += '\x02';  // RGB
    header += std::string(3, '\0');
    return std::string("\x89PNG\r\n\x1a\n", 8) + Chunk("IHDR", header) +
           Chunk("IDAT", zlib) + Chunk("IEND", "");
}

// ---------------------------------------------------------------------
// The fake fal.ai: a plain function, as the seam takes one.
// ---------------------------------------------------------------------

struct FakeFal {
    std::vector<generation::HttpRequest> requests;
    int status_reads_before_completed = 0;
    int submit_status = 200;
    std::string submit_body = R"({"request_id":"REQ1","status":"IN_QUEUE"})";
    std::string result_body;
    std::string picture;
    bool always_in_queue = false;
    // PA12 -- what a creative kind's result URL serves.
    std::string video;
};

FakeFal& Fake() {
    static FakeFal fake;
    return fake;
}

bool HasKey(const generation::HttpRequest& request) {
    for (const auto& header : request.headers)
        if (header.first == "Authorization") return true;
    return false;
}

std::string KeyOf(const generation::HttpRequest& request) {
    for (const auto& header : request.headers)
        if (header.first == "Authorization") return header.second;
    return {};
}

bool FakeHttp(const generation::HttpRequest& request,
              generation::HttpResponse& response, std::string& error) {
    FakeFal& fake = Fake();
    fake.requests.push_back(request);
    const std::string& url = request.url;
    response = {};
    response.status = 200;
    if (url.rfind("https://rest.fal.ai/storage/upload/initiate", 0) == 0) {
        response.body =
            R"({"upload_url":"https://storage.example/put/source","file_url":"https://v3.fal.media/files/source.png"})";
    } else if (url == "https://storage.example/put/source") {
        response.body = "";
    } else if (url == "https://queue.fal.run/fal-ai/bria/expand") {
        response.status = fake.submit_status;
        response.body = fake.submit_body;
    } else if (url ==
               "https://queue.fal.run/fal-ai/bria/requests/REQ1/status") {
        const bool done =
            !fake.always_in_queue && fake.status_reads_before_completed-- <= 0;
        response.body = done
                            ? R"({"status":"COMPLETED","request_id":"REQ1"})"
                            : R"({"status":"IN_PROGRESS","request_id":"REQ1"})";
    } else if (url == "https://queue.fal.run/fal-ai/bria/requests/REQ1") {
        response.body = fake.result_body;
    } else if (url ==
               "https://queue.fal.run/fal-ai/bria/requests/REQ1/cancel") {
        response.status = 202;
    } else if (url == "https://v3.fal.media/files/expanded.png") {
        response.body = fake.picture;
        response.content_type = "image/png";
    } else if (url == "https://v3.fal.media/files/made.png") {
        response.body = fake.picture;
        response.content_type = "image/png";
    } else if (url == "https://v3.fal.media/files/made.mp4") {
        response.body = fake.video;
        response.content_type = "video/mp4";
    } else if (url.rfind("https://queue.fal.run/", 0) == 0 && url.size() > 21 &&
               url.compare(url.size() - 21, 21, "/requests/REQ1/status") == 0) {
        // PA12 -- any other model's queue, by the same protocol.
        response.body = R"({"status":"COMPLETED","request_id":"REQ1"})";
    } else if (url.rfind("https://queue.fal.run/", 0) == 0 && url.size() > 14 &&
               url.compare(url.size() - 14, 14, "/requests/REQ1") == 0) {
        response.body = fake.result_body;
    } else if (url.rfind("https://queue.fal.run/", 0) == 0) {
        response.status = fake.submit_status;
        response.body = fake.submit_body;
    } else {
        error = "unexpected request " + request.method + " " + url;
        return false;
    }
    return true;
}

bool FakeKeychain(std::string& key, std::string& /*message*/) {
    key = "kc-secret-0123456789";
    return true;
}

generation::HttpTransport FakeTransport() {
    return [](const generation::HttpRequest& request,
              generation::HttpResponse& response, std::string& error) {
        return FakeHttp(request, response, error);
    };
}

constexpr char kKey[] = "fal-test-key-abcdef123456";

// PA14 -- a rush FFmpeg draws: `seconds` of a test picture with a tone.
bool MakeRush(const fs::path& path, int width, int height, int seconds,
              std::string& detail) {
    process::Request request;
    request.arguments = {"",
                         "-nostdin",
                         "-v",
                         "error",
                         "-f",
                         "lavfi",
                         "-i",
                         "testsrc2=size=" + std::to_string(width) + "x" +
                             std::to_string(height) +
                             ":rate=25:duration=" + std::to_string(seconds),
                         "-f",
                         "lavfi",
                         "-i",
                         "sine=frequency=440:sample_rate=48000:duration=" +
                             std::to_string(seconds),
                         "-c:v",
                         "libx264",
                         "-preset",
                         "ultrafast",
                         "-pix_fmt",
                         "yuv420p",
                         "-c:a",
                         "aac",
                         "-shortest",
                         "-y",
                         path.string()};
    request.standard_output = process::Stream::kDiscard;
    request.standard_error = process::Stream::kPipe;
    request.discard_input = true;
    const process::Result made = ffmpeg::Run(std::move(request));
    detail = made.error_tail;
    return made.Started() && made.Succeeded();
}

}  // namespace

int main() {
    Test("the extension canvas: exact aspect, centred, the model's bounds", [] {
        generation::ExpandCanvas canvas;
        std::string message;
        Check(
            generation::ComputeExpandCanvas(1920, 1080, 9, 16, canvas, message),
            "16:9 to 9:16: " + message);
        Check(canvas.canvas_width == 1926 && canvas.canvas_height == 3424 &&
                  canvas.x == 3 && canvas.y == 1172 &&
                  canvas.image_width == 1920 && canvas.image_height == 1080,
              "1926x3424 (214 x 9:16), the picture at (3, 1172)");
        Check(canvas.canvas_width * 16 == canvas.canvas_height * 9,
              "the aspect is exact, not rounded");
        Check(generation::ComputeExpandCanvas(1080, 1920, 32, 18, canvas,
                                              message) &&
                  canvas.canvas_width == 3424 && canvas.canvas_height == 1926,
              "9:16 to 16:9, with an unreduced 32:18");
        Check(generation::ComputeExpandCanvas(1001, 500, 16, 9, canvas,
                                              message) &&
                  canvas.canvas_width == 1024 && canvas.canvas_height == 576,
              "an odd multiple moves to the next even one: both sides even, "
              "for the 4:2:0 clip");
        Check(!generation::ComputeExpandCanvas(1920, 1080, 16, 9, canvas,
                                               message) &&
                  message.find("déjà") != std::string::npos,
              "the same aspect is refused: " + message);
        Check(!generation::ComputeExpandCanvas(4000, 3000, 9, 16, canvas,
                                               message) &&
                  message.find("5000") != std::string::npos,
              "a canvas past 5000 x 5000 is refused: " + message);
        Check(!generation::ComputeExpandCanvas(1000, 100, 1, 1, canvas,
                                               message) &&
                  message.find("15 %") != std::string::npos,
              "a picture under 15 % of its canvas is refused: " + message);
        int64_t width = 0;
        int64_t height = 0;
        Check(generation::ParseAspectRatio("9:16", width, height, message) &&
                  width == 9 && height == 16,
              "9:16 reads");
        for (const char* bad : {"9/16", "0:1", "a:b", "9:", ":16", "916"}) {
            Check(!generation::ParseAspectRatio(bad, width, height, message),
                  std::string("refused: ") + bad);
        }
    });

    Test("the request and the answer of fal-ai/bria/expand", [] {
        generation::ExpandCanvas canvas;
        std::string message;
        generation::ComputeExpandCanvas(1920, 1080, 9, 16, canvas, message);
        Check(
            generation::ExpandInput(canvas, "", "", "").Dump() ==
                R"({"canvas_size":[1926,3424],"original_image_size":[1920,1080],"original_image_location":[3,1172]})",
            "only what was asked: " +
                generation::ExpandInput(canvas, "", "", "").Dump());
        Check(
            generation::ExpandInput(canvas, "a beach", "text", "42").Dump() ==
                R"({"canvas_size":[1926,3424],"original_image_size":[1920,1080],"original_image_location":[3,1172],"prompt":"a beach","negative_prompt":"text","seed":42})",
            "prompt, negative prompt and seed as the model names them");
        std::string url;
        std::string seed;
        int64_t width = 0;
        int64_t height = 0;
        Check(
            generation::ParseExpandOutput(
                R"({"image":{"url":"https://v3.fal.media/files/a.png","width":1926,"height":3424,"content_type":"image/png"},"seed":1234})",
                url, seed, width, height, message) &&
                url == "https://v3.fal.media/files/a.png" && seed == "1234" &&
                width == 1926 && height == 3424,
            "the picture, its seed and size");
        Check(!generation::ParseExpandOutput(
                  R"({"image":{"url":"http://insecure/a.png"}})", url, seed,
                  width, height, message),
              "an address that is not https is refused");
        Check(!generation::ParseExpandOutput(R"({"images":[]})", url, seed,
                                             width, height, message),
              "an answer without `image` is refused");
        Check(generation::FalQueueAppPath("fal-ai/bria/expand") ==
                      "fal-ai/bria" &&
                  generation::FalQueueAppPath("fal-ai/flux") == "fal-ai/flux" &&
                  generation::FalQueueAppPath("workflows/me/flow/run") ==
                      "workflows/me/flow",
              "the queue's path keeps owner and alias, as the client does");
    });

    Test("fal.ai's queue: the key to fal.ai only, polling, the result", [] {
        Fake() = FakeFal{};
        Fake().status_reads_before_completed = 2;
        Fake().result_body =
            R"({"image":{"url":"https://v3.fal.media/files/expanded.png"},"seed":7})";
        Fake().picture = "PNGBYTES";
        int slept = 0;
        generation::FalSettings settings;
        settings.sleep = [&slept](double) { ++slept; };
        const generation::FalClient client(FakeTransport(), kKey, settings);
        std::string fileUrl;
        std::string message;
        Check(client.Upload("SOURCE", "image/png", "src.png", fileUrl, message),
              "upload: " + message);
        Check(fileUrl == "https://v3.fal.media/files/source.png",
              "the URL the model reads");
        std::string result;
        std::string requestId;
        Check(
            client.Run("fal-ai/bria/expand", Parse(R"({"canvas_size":[2,4]})"),
                       result, requestId, message),
            "run: " + message);
        Check(requestId == "REQ1" && slept == 2,
              "two waits while in progress, then the result");
        std::string bytes;
        std::string type;
        Check(client.Download("https://v3.fal.media/files/expanded.png", bytes,
                              type, message) &&
                  bytes == "PNGBYTES" && type == "image/png",
              "the picture fetched: " + message);
        const auto& requests = Fake().requests;
        Check(requests.size() == 8,
              "initiate, put, submit, 3 status, "
              "result, download: " +
                  std::to_string(requests.size()));
        for (const auto& request : requests) {
            const bool falHost =
                request.url.rfind("https://queue.fal.run/", 0) == 0 ||
                request.url.rfind("https://rest.fal.ai/", 0) == 0;
            Check(HasKey(request) == falHost,
                  "the key goes to fal.ai's hosts and nowhere else: " +
                      request.url);
            if (falHost)
                Check(KeyOf(request) == std::string("Key ") + kKey,
                      "as `Key <key>`");
        }
        if (requests.size() == 8) {
            Check(
                requests[0].method == "POST" &&
                    Parse(requests[0].body).Dump() ==
                        R"({"content_type":"image/png","file_name":"src.png"})",
                "the upload is initiated with its type and name");
            Check(requests[1].method == "PUT" && requests[1].body == "SOURCE",
                  "then the bytes are PUT, without the key");
            Check(requests[2].method == "POST" &&
                      requests[2].body == R"({"canvas_size":[2,4]})",
                  "the input is submitted as given");
            Check(requests[6].method == "GET" &&
                      requests[6].url ==
                          "https://queue.fal.run/fal-ai/bria/requests/REQ1",
                  "the result read from the owner/alias path");
        }
    });

    Test(
        "fal.ai's refusals: the key never in a message, a stuck request "
        "cancelled",
        [] {
            Fake() = FakeFal{};
            Fake().submit_status = 401;
            Fake().submit_body =
                std::string(R"({"detail":"bad key )") + kKey + R"("})";
            generation::FalSettings settings;
            settings.sleep = [](double) {};
            const generation::FalClient client(FakeTransport(), kKey, settings);
            std::string result;
            std::string requestId;
            std::string message;
            Check(!client.Run("fal-ai/bria/expand", Parse("{}"), result,
                              requestId, message),
                  "a 401 is a refusal");
            Check(message.find(kKey) == std::string::npos &&
                      message.find("401") != std::string::npos &&
                      message.find("clé masquée") != std::string::npos,
                  "said, without the key: " + message);

            Fake() = FakeFal{};
            Fake().always_in_queue = true;
            settings.max_wait_seconds = 3;
            const generation::FalClient patient(FakeTransport(), kKey,
                                                settings);
            Check(!patient.Run("fal-ai/bria/expand", Parse("{}"), result,
                               requestId, message) &&
                      message.find("annulée") != std::string::npos,
                  "past its wait, refused: " + message);
            Check(!Fake().requests.empty() &&
                      Fake().requests.back().method == "PUT" &&
                      Fake().requests.back().url ==
                          "https://queue.fal.run/fal-ai/bria/requests/REQ1/"
                          "cancel",
                  "and cancelled at fal.ai");

            const generation::FalClient none({}, kKey, settings);
            Check(!none.Run("fal-ai/bria/expand", Parse("{}"), result,
                            requestId, message) &&
                      message == generation::kNoTransportMessage,
                  "no transport: refused by name");
            Check(generation::RedactSecret("a short key", "short") ==
                      "a short key",
                  "a short secret is not hunted through ordinary words");
        });

    Test("the key: the secret store first, FAL_KEY second, or how to set one",
         [] {
             generation::SetGenerationKeyReader(nullptr);
             unsetenv("FAL_KEY");
             std::string key;
             std::string origin;
             std::string message;
             Check(!generation::ResolveFalKey(key, origin, message) &&
                       message.find("security add-generic-password -s "
                                    "com.cutmachine.generation -a "
                                    "fal-api-key -w") != std::string::npos &&
                       message.find("FAL_KEY") != std::string::npos,
                   "none: says how to store one: " + message);
             setenv("FAL_KEY", "  env-key-0123456789  ", 1);
             Check(generation::ResolveFalKey(key, origin, message) &&
                       key == "env-key-0123456789" && origin == "FAL_KEY",
                   "FAL_KEY, trimmed");
             generation::SetGenerationKeyReader(&FakeKeychain);
             Check(generation::ResolveFalKey(key, origin, message) &&
                       key == "kc-secret-0123456789" && origin == "trousseau",
                   "the secret store wins");
             generation::SetGenerationKeyReader(nullptr);
             unsetenv("FAL_KEY");
         });

    Test("a proposal is spent once, and not after its lifetime", [] {
        generation::GenerationProposalStore store;
        generation::GenerationProposal proposal;
        proposal.id = GenerateUlid();
        proposal.created = std::chrono::steady_clock::now();
        std::string message;
        generation::GenerationProposal taken;
        Check(!store.Take(proposal.id, proposal.created, taken, message) &&
                  message.find("aucune proposition") != std::string::npos,
              "unknown: " + message);
        store.Add(proposal);
        Check(store.Peek(proposal.id, proposal.created, taken, message) &&
                  store.Size() == 1,
              "a look leaves it in the store");
        Check(store.Take(proposal.id, proposal.created, taken, message) &&
                  taken.id == proposal.id,
              "taken once");
        Check(!store.Take(proposal.id, proposal.created, taken, message) &&
                  message.find("déjà servi") != std::string::npos,
              "not twice: " + message);
        generation::GenerationProposal old = proposal;
        old.id = GenerateUlid();
        store.Add(old);
        Check(!store.Take(
                  old.id,
                  old.created + std::chrono::minutes(
                                    generation::kProposalLifetimeMinutes + 1),
                  taken, message) &&
                  message.find("expiré") != std::string::npos,
              "expired: " + message);
        Check(ToolSpendsOnPersonsAccount("generate_media") &&
                  !ToolSpendsOnPersonsAccount("propose_generation") &&
                  !ToolSpendsOnPersonsAccount("read_frame"),
              "only generate_media spends");
    });

    Test("SetMediaGeneration: version 20, undo byte for byte, the journal", [] {
        Document document;
        LibraryMedia media;
        media.path = "generated/a.png";
        media.filename = "a.png";
        media.codec = "png";
        media.width = 1926;
        media.height = 3424;
        media.rate = {25, 1};
        media.duration = {1, 25};
        media.orientation = "portrait";
        document.library.push_back(media);
        const std::string initial = document.SaveToString();
        Check(initial.find("\"generation\"") == std::string::npos &&
                  initial.find("\"version\": 7") != std::string::npos,
              "a filmed library writes no key and keeps version 7");
        MediaGeneration provenance;
        provenance.service = "fal.ai";
        provenance.model = "fal-ai/bria/expand";
        provenance.kind = "expand_image";
        provenance.prompt = "une plage";
        provenance.seed = "1234";
        provenance.created_at = "2026-10-21T15:04:05Z";
        provenance.request_id = "REQ1";
        provenance.source_media_id = "01K20000000000000000000001";
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document,
                        SetMediaGenerationOperation{media.id, provenance},
                        error, message),
              "applies: " + message);
        const std::string marked = document.SaveToString();
        Check(marked.find("\"version\": 20") != std::string::npos &&
                  marked.find("\"generation\":{\"service\":\"fal.ai\"") !=
                      std::string::npos &&
                  marked.find("announced_cost") == std::string::npos,
              "written, version 20, an unknown cost left out:\n" + marked);
        Document reread;
        Check(Document::LoadFromString(marked, reread, message) &&
                  reread.library.size() == 1 &&
                  reread.library[0].generation == provenance &&
                  reread.SaveToString() == marked,
              "read back as written: " + message);
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<SetMediaGenerationOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "SetMediaGeneration round-trips through the journal exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "undo gives back the version-7 document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == marked,
              "redo gives back the marked one");

        // On a library still unmarked: the guard of DEC36 below would
        // otherwise refuse first, for another reason.
        MediaGeneration partial;
        partial.service = "fal.ai";
        Document copy;
        EditLog fresh;
        Check(
            Document::LoadFromString(initial, copy, message) &&
                !fresh.Apply(copy,
                             SetMediaGenerationOperation{media.id, partial},
                             error, message) &&
                error == EditError::ValidationFailed &&
                message.find("generation") != std::string::npos,
            "a provenance without model, kind or date is refused: " + message);
        Check(!log.Apply(copy,
                         SetMediaGenerationOperation{
                             "01K20000000000000000000099", provenance},
                         error, message) &&
                  error == EditError::UnknownMedia,
              "an unknown media is refused");

        // DEC36 -- docs/roadmap/GENERATIF-2026-10.md. A provenance is
        // erased by undo only: an edit that clears it or replaces it is
        // refused, and leaves the document and the journal as they were.
        const size_t entries = log.AppliedCount();
        Check(!log.Apply(document, SetMediaGenerationOperation{media.id, {}},
                         error, message) &&
                  error == EditError::InvalidOperation &&
                  message.find("only undo removes it") != std::string::npos &&
                  document.SaveToString() == marked &&
                  log.AppliedCount() == entries,
              "an edit does not clear a provenance: " + message);
        MediaGeneration other = provenance;
        other.prompt = "a snowy beach";
        Check(!log.Apply(document, SetMediaGenerationOperation{media.id, other},
                         error, message) &&
                  error == EditError::InvalidOperation &&
                  document.SaveToString() == marked,
              "nor replaces it with another: " + message);
        Check(
            !log.Apply(document,
                       BatchOperation{
                           "lot", {SetMediaGenerationOperation{media.id, {}}}},
                       error, message) &&
                document.SaveToString() == marked,
            "nor does a batch: " + message);
        Document blank;
        EditLog twice;
        Check(
            Document::LoadFromString(initial, blank, message) &&
                !twice.Apply(
                    blank,
                    BatchOperation{
                        "pose et retire",
                        {SetMediaGenerationOperation{media.id, provenance},
                         SetMediaGenerationOperation{media.id, {}}}},
                    error, message) &&
                blank.SaveToString() == initial,
            "a batch that sets one and clears it is refused whole: " + message);
        Check(log.Apply(document,
                        SetMediaGenerationOperation{media.id, provenance},
                        error, message) &&
                  document.SaveToString() == marked,
              "the same provenance again changes nothing and is let "
              "through: " +
                  message);
        Check(log.Undo(document, error, message) &&
                  log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "undo still takes it away, byte for byte: " + message);
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == marked,
              "and redo puts it back: " + message);
        // DEC32 (PRD7) -- the inverse of an ingest, applied as a new edit,
        // would take the media and its provenance out of the library.
        LibraryMedia still = media;
        still.id = GenerateUlid();
        Document ingested;
        EditLog journal;
        Check(
            journal.Apply(ingested, IngestMediaOperation{{still}, {}, {}},
                          error, message) &&
                journal.Apply(ingested,
                              SetMediaGenerationOperation{still.id, provenance},
                              error, message),
            "a still ingested, then marked: " + message);
        const std::string ingestedBytes = ingested.SaveToString();
        const size_t journaled = journal.AppliedCount();
        Check(!journal.Apply(ingested,
                             RemoveIngestedMediaOperation{{still.id}, {}, {}},
                             error, message) &&
                  error == EditError::InvalidOperation &&
                  message.find("only undo removes it") != std::string::npos &&
                  ingested.SaveToString() == ingestedBytes &&
                  journal.AppliedCount() == journaled,
              "nor does taking the ingest back by an edit: " + message);
        Check(journal.Undo(ingested, error, message) &&
                  journal.Undo(ingested, error, message) &&
                  ingested.library.empty() && ingested.sources.empty(),
              "undo still takes the mark and the ingest back: " + message);
    });

    Test("propose_generation then generate_media, on a project", [] {
        const fs::path root = fs::temp_directory_path() /
                              ("cutmachine-generation-" + GenerateUlid());
        fs::create_directories(root);
        std::string path;
        std::string detail;
        Check(CreatePortableProject((root / "film").string(), Project("Film"),
                                    path, detail),
              "project: " + detail);
        // A rush of ten frames at 25/1, made from a PNG by the same
        // encoder that makes a generated clip.
        const fs::path still = root / "rush.png";
        {
            std::ofstream out(still, std::ios::binary);
            out << Png(32, 18);
        }
        const fs::path rush = root / "rush.mp4";
        std::string encodeMessage;
        Check(generation::EncodeStillClip(still.string(), rush.string(),
                                          MediaRate{25, 1}, 10, encodeMessage),
              "the rush is encoded: " + encodeMessage);
        Check(!generation::EncodeStillClip(still.string(), rush.string(),
                                           MediaRate{25, 1}, 10, encodeMessage),
              "an existing clip is never overwritten");
        McpProjectBackend backend(path);
        McpToolRegistry registry;
        Value ingest = Value::MakeObject();
        ingest.Set("path", Value::MakeString(rush.string()));
        const auto ingested = registry.Call(backend, "ingest_media", ingest);
        Check(ingested.ok, "the rush is ingested: " + ingested.message);
        Document document;
        std::string message;
        backend.SnapshotDocument(document, message);
        Check(document.library.size() == 1, "one media");
        if (document.library.size() != 1) return;
        const LibraryMedia source = document.library[0];
        Check(source.width == 32 && source.height == 18,
              "read as 32x18: " + std::to_string(source.width) + "x" +
                  std::to_string(source.height));

        generation::SetGenerationTransport(nullptr);
        generation::SetGenerationKeyReader(nullptr);
        unsetenv("FAL_KEY");
        const auto propose = [&](const std::string& aspect) {
            Value args = Value::MakeObject();
            args.Set("kind", Value::MakeString("expand_image"));
            args.Set("media_id", Value::MakeString(source.id));
            args.Set("aspect_ratio", Value::MakeString(aspect));
            args.Set("prompt", Value::MakeString("a sandy beach"));
            args.Set("source_time", Parse(R"({"value":0,"rate":25})"));
            return registry.Call(backend, "propose_generation", args);
        };
        const auto proposed = propose("9:16");
        Check(proposed.ok, "proposed: " + proposed.message);
        const Value proposal = Parse(proposed.result_json);
        const std::string id = At(proposal, {"proposal_id"});
        Check(!id.empty() && At(proposal, {"spends"}) == "false" &&
                  At(proposal, {"model"}) == "fal-ai/bria/expand" &&
                  At(proposal, {"announced_cost"}) == "null" &&
                  !At(proposal, {"cost_note"}).empty() &&
                  At(proposal, {"request", "canvas_size"}) == "[36,64]" &&
                  At(proposal, {"request", "original_image_location"}) ==
                      "[2,23]" &&
                  At(proposal, {"ready", "key"}) == "false" &&
                  At(proposal, {"ready", "transport"}) == "false" &&
                  At(proposal, {"next"}).find("Ne confirme jamais") !=
                      std::string::npos,
              "what would be sent, unknown cost said, nothing ready: " +
                  proposed.result_json);
        Check(!propose("16:9").ok,
              "a 16:9 picture has nothing to extend "
              "to 16:9");
        Value noTime = Value::MakeObject();
        noTime.Set("kind", Value::MakeString("expand_image"));
        noTime.Set("media_id", Value::MakeString(source.id));
        noTime.Set("aspect_ratio", Value::MakeString("9:16"));
        const auto timeless =
            registry.Call(backend, "propose_generation", noTime);
        Check(!timeless.ok &&
                  timeless.message.find("source_time") != std::string::npos,
              "a video needs the frame named: " + timeless.message);
        Value badKind = Value::MakeObject();
        badKind.Set("kind", Value::MakeString("voice_over"));
        badKind.Set("media_id", Value::MakeString(source.id));
        badKind.Set("aspect_ratio", Value::MakeString("9:16"));
        badKind.Set("source_time", Parse(R"({"value":0,"rate":25})"));
        Check(!registry.Call(backend, "propose_generation", badKind).ok,
              "a kind that is not built is refused");

        const auto generate = [&](const std::string& proposalId,
                                  bool confirmed) {
            Value args = Value::MakeObject();
            args.Set("proposal_id", Value::MakeString(proposalId));
            args.Set("confirmed_by_person", Value::MakeBool(confirmed));
            return registry.Call(backend, "generate_media", args);
        };
        Value unconfirmed = Value::MakeObject();
        unconfirmed.Set("proposal_id", Value::MakeString(id));
        const auto noConfirmation =
            registry.Call(backend, "generate_media", unconfirmed);
        Check(!noConfirmation.ok &&
                  noConfirmation.error_name == "ConfirmationRequired" &&
                  generate(id, false).error_name == "ConfirmationRequired",
              "without the person's confirmation, refused");
        const auto noTransport = generate(id, true);
        Check(!noTransport.ok &&
                  noTransport.error_name == "UnsupportedOperation" &&
                  noTransport.message.find("nothing was spent") !=
                      std::string::npos,
              "without a transport, refused, nothing spent: " +
                  noTransport.message);
        generation::SetGenerationTransport(&FakeHttp);
        const auto noKey = generate(id, true);
        Check(!noKey.ok && noKey.error_name == "GenerationKeyMissing",
              "without a key, refused: " + noKey.message);

        Fake() = FakeFal{};
        Fake().result_body =
            R"({"image":{"url":"https://v3.fal.media/files/expanded.png","width":36,"height":64},"seed":99})";
        Fake().picture = Png(36, 64);
        setenv("FAL_KEY", kKey, 1);
        const auto generated = generate(id, true);
        Check(generated.ok, "generated, on the same proposal: " +
                                generated.error_name + " " + generated.message);
        const Value answer =
            generated.ok ? Parse(generated.result_json) : Value::MakeObject();
        const std::string mediaId = At(answer, {"media_id"});
        Check(At(answer, {"spent"}) == "true" &&
                  At(answer, {"generation", "seed"}) == "99" &&
                  At(answer, {"generation", "request_id"}) == "REQ1" &&
                  At(answer, {"key_from"}) == "FAL_KEY",
              "spent, with the service's seed and request: " +
                  generated.result_json);
        const fs::path written = At(answer, {"path"});
        const fs::path picture = At(answer, {"picture"});
        Check(written.extension() == ".mp4" && picture.extension() == ".png" &&
                  fs::exists(picture) &&
                  written.parent_path() == picture.parent_path(),
              "the service's picture kept, a clip made beside it");
        Check(written.parent_path().filename() == "generated" &&
                  written.parent_path().parent_path() ==
                      fs::path(path).parent_path() &&
                  fs::exists(written),
              "the picture is in the package: " + written.string());
        for (const auto& request : Fake().requests) {
            Check(request.body.find(kKey) == std::string::npos,
                  "no body carries the key");
        }
        Check(Fake().requests.size() >= 3 &&
                  Parse(Fake().requests[2].body).Find("image_url") != nullptr &&
                  At(Parse(Fake().requests[2].body), {"image_url"}) ==
                      "https://v3.fal.media/files/source.png" &&
                  At(Parse(Fake().requests[2].body), {"prompt"}) ==
                      "a sandy beach",
              "the model reads the uploaded source and the prompt");
        Check(!Fake().requests.empty() &&
                  At(Parse(Fake().requests[0].body), {"content_type"}) ==
                      "image/jpeg" &&
                  Fake().requests[1].body.rfind("\xff\xd8", 0) == 0,
              "the frame of the rush is sent as a JPEG");

        Document after;
        backend.SnapshotDocument(after, message);
        const LibraryMedia* made = after.FindLibraryMedia(mediaId);
        Check(made != nullptr && made->width == 36 && made->height == 64 &&
                  made->duration.value * 25 == 125 * made->duration.rate &&
                  made->generation.service == "fal.ai" &&
                  made->generation.model == "fal-ai/bria/expand" &&
                  made->generation.prompt == "a sandy beach" &&
                  made->generation.source_media_id == source.id &&
                  made->generation.created_at.size() == 20,
              "an ordinary media of the library, marked");
        Value describeArgs = Value::MakeObject();
        const auto described = registry.Call(backend, "describe", describeArgs);
        Check(described.ok && described.result_json.find(
                                  "\"generation\":{\"service\":\"fal.ai\"") !=
                                  std::string::npos,
              "describe shows the provenance");
        std::ifstream projectFile(path);
        std::stringstream projectText;
        projectText << projectFile.rdbuf();
        Check(projectText.str().find("\"document_version\":20") !=
                  std::string::npos,
              "the project declares version 20");

        const auto again = generate(id, true);
        Check(!again.ok && again.error_name == "ProposalUnavailable" &&
                  again.message.find("déjà servi") != std::string::npos,
              "the proposal does not spend twice: " + again.message);

        generation::SetGenerationTransport(nullptr);
        unsetenv("FAL_KEY");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    });

    // PA12 -- docs/roadmap/PARITE-2026-11.md. The creative kinds, through
    // the catalog, against the fake fal.ai: what each proposal would send,
    // what needs the filmed person's consent, what enters the library.
    Test("PA12: the creative kinds, proposed then generated", [] {
        const fs::path root = fs::temp_directory_path() /
                              ("cutmachine-creative-" + GenerateUlid());
        fs::create_directories(root);
        std::string path;
        std::string detail;
        Check(CreatePortableProject((root / "film").string(), Project("Film"),
                                    path, detail),
              "project: " + detail);
        // Two seconds of a rush with a voice on it.
        const fs::path rush = root / "rush.mp4";
        {
            process::Request request;
            request.arguments = {
                "",           "-nostdin",
                "-v",         "error",
                "-f",         "lavfi",
                "-i",         "testsrc2=size=64x36:rate=25:duration=2",
                "-f",         "lavfi",
                "-i",         "sine=frequency=440:sample_rate=48000:duration=2",
                "-c:v",       "libx264",
                "-pix_fmt",   "yuv420p",
                "-c:a",       "aac",
                "-shortest",  "-y",
                rush.string()};
            request.standard_output = process::Stream::kDiscard;
            request.standard_error = process::Stream::kPipe;
            request.discard_input = true;
            const process::Result made = ffmpeg::Run(std::move(request));
            Check(made.Started() && made.Succeeded(),
                  "the rush is made: " + made.error_tail);
        }
        McpProjectBackend backend(path);
        McpToolRegistry registry;
        Value ingest = Value::MakeObject();
        ingest.Set("path", Value::MakeString(rush.string()));
        Check(registry.Call(backend, "ingest_media", ingest).ok,
              "the rush is ingested");
        Document document;
        std::string message;
        backend.SnapshotDocument(document, message);
        if (document.library.size() != 1) {
            Check(false, "one media");
            return;
        }
        const LibraryMedia source = document.library[0];
        Check(source.has_audio && source.has_video, "a rush with its sound");

        generation::SetGenerationTransport(&FakeHttp);
        generation::SetGenerationKeyReader(nullptr);
        setenv("FAL_KEY", kKey, 1);
        std::string rushBytes;
        {
            std::ifstream in(rush, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            rushBytes = buffer.str();
        }
        const auto propose = [&](const std::string& json) {
            return registry.Call(backend, "propose_generation", Parse(json));
        };
        const auto generate = [&](const std::string& id, bool consent) {
            Value args = Value::MakeObject();
            args.Set("proposal_id", Value::MakeString(id));
            args.Set("confirmed_by_person", Value::MakeBool(true));
            if (consent)
                args.Set("consent_of_filmed_person", Value::MakeBool(true));
            return registry.Call(backend, "generate_media", args);
        };
        // The body of the request submitted to the model: the last POST
        // to the queue that is neither a status read nor a result read.
        const auto submitted = [&]() {
            Value body = Value::MakeObject();
            for (const auto& request : Fake().requests) {
                if (request.method == "POST" &&
                    request.url.rfind("https://queue.fal.run/", 0) == 0 &&
                    request.url.find("/requests/") == std::string::npos)
                    body = Parse(request.body);
            }
            return body;
        };
        const auto mediaOf = [&](const Value& answer) {
            Document now;
            backend.SnapshotDocument(now, message);
            const LibraryMedia* made =
                now.FindLibraryMedia(At(answer, {"media_id"}));
            return made != nullptr ? *made : LibraryMedia{};
        };

        // ---- a shot made from words ----
        const auto shot =
            propose(R"({"kind":"stock_video","prompt":"a beach at dawn",)"
                    R"("duration_seconds":6})");
        Check(shot.ok, "stock_video is proposed: " + shot.message);
        const Value shotView = shot.ok ? Parse(shot.result_json) : Value();
        Check(At(shotView, {"model"}) ==
                      "fal-ai/kling-video/v3/pro/text-to-video" &&
                  At(shotView, {"spends"}) == "false" &&
                  At(shotView, {"needs_consent"}) == "false" &&
                  At(shotView, {"request", "duration"}) == "6" &&
                  At(shotView, {"request", "aspect_ratio"}) == "16:9" &&
                  At(shotView, {"request", "generate_audio"}) == "false" &&
                  At(shotView, {"announced_cost"}).find("0,112") !=
                      std::string::npos,
              "it says the model, the request and the price: " +
                  shot.result_json);
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
        Fake().video = rushBytes;
        const auto shotMade = generate(At(shotView, {"proposal_id"}), false);
        Check(shotMade.ok, "stock_video is generated: " + shotMade.error_name +
                               " " + shotMade.message);
        if (shotMade.ok) {
            const Value answer = Parse(shotMade.result_json);
            const LibraryMedia made = mediaOf(answer);
            Check(fs::path(At(answer, {"path"})).extension() == ".mp4" &&
                      fs::path(At(answer, {"path"})).parent_path().filename() ==
                          "generated" &&
                      made.generation.kind == "stock_video" &&
                      made.generation.prompt == "a beach at dawn" &&
                      made.generation.request_id == "REQ1" && made.has_video,
                  "the shot is in the package and the library, marked");
            Check(At(submitted(), {"prompt"}) == "a beach at dawn" &&
                      submitted().Find("video_url") == nullptr,
                  "the model reads the prompt and no file");
            for (const auto& request : Fake().requests)
                Check(request.body.find(kKey) == std::string::npos,
                      "no body carries the key");
        }
        Check(!propose(R"({"kind":"stock_video"})").ok,
              "a shot with no prompt is refused");
        Check(!propose(R"({"kind":"stock_video","prompt":"x",)"
                       R"("duration_seconds":2})")
                   .ok,
              "a shot shorter than the model makes is refused");
        Check(!propose(R"({"kind":"stock_video","prompt":"x",)"
                       R"("aspect_ratio":"4:5"})")
                   .ok,
              "a shape the model does not make is refused");
        const auto stray = propose(
            R"({"kind":"stock_video","prompt":"x","target_language":"en"})");
        Check(
            !stray.ok && stray.message.find("is not read by 'stock_video'") !=
                             std::string::npos,
            "an argument of another kind is refused by name: " + stray.message);

        // ---- a picture made from words ----
        const auto picture =
            propose(R"({"kind":"stock_image","prompt":"a red door","seed":7})");
        Check(picture.ok, "stock_image is proposed: " + picture.message);
        const Value pictureView =
            picture.ok ? Parse(picture.result_json) : Value();
        Check(
            At(pictureView, {"model"}) == "fal-ai/flux-2-pro" &&
                At(pictureView, {"request", "image_size", "width"}) == "1920" &&
                At(pictureView, {"request", "image_size", "height"}) ==
                    "1080" &&
                At(pictureView, {"request", "seed"}) == "7",
            "at the timeline's size, with its seed: " + picture.result_json);
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"images":[{"url":"https://v3.fal.media/files/made.png","width":64,"height":36}],"seed":7})";
        Fake().picture = Png(64, 36);
        const auto pictureMade =
            generate(At(pictureView, {"proposal_id"}), false);
        Check(pictureMade.ok,
              "stock_image is generated: " + pictureMade.error_name + " " +
                  pictureMade.message);
        Ulid generatedPicture;
        if (pictureMade.ok) {
            const Value answer = Parse(pictureMade.result_json);
            const LibraryMedia made = mediaOf(answer);
            generatedPicture = made.id;
            Check(fs::path(At(answer, {"path"})).extension() == ".png" &&
                      made.generation.kind == "stock_image" &&
                      made.generation.seed == "7" && made.width == 64,
                  "the picture enters the library as a still, marked");
        }

        // ---- the voices in another language: a filmed person ----
        const auto dub = propose(R"({"kind":"dubbing","media_id":")" +
                                 source.id + R"(","target_language":"en"})");
        Check(dub.ok, "dubbing is proposed: " + dub.message);
        const Value dubView = dub.ok ? Parse(dub.result_json) : Value();
        Check(
            At(dubView, {"needs_consent"}) == "true" &&
                At(dubView, {"request", "target_lang"}) == "en" &&
                At(dubView, {"uploads", "0", "what"}) == "excerpt" &&
                At(dubView, {"uploads", "0", "field"}) == "video_url" &&
                At(dubView, {"next"}).find("consent_of_filmed_person") !=
                    std::string::npos,
            "it sends an excerpt and asks for the consent: " + dub.result_json);
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"},"target_lang":"en"})";
        Fake().video = rushBytes;
        const auto withoutConsent =
            generate(At(dubView, {"proposal_id"}), false);
        Check(!withoutConsent.ok &&
                  withoutConsent.error_name == "ConsentRequired" &&
                  withoutConsent.message.find("nothing was spent") !=
                      std::string::npos &&
                  Fake().requests.empty(),
              "without the filmed person's consent nothing leaves: " +
                  withoutConsent.message);
        const auto dubbed = generate(At(dubView, {"proposal_id"}), true);
        Check(dubbed.ok, "with it, the same proposal runs: " +
                             dubbed.error_name + " " + dubbed.message);
        if (dubbed.ok) {
            const Value answer = Parse(dubbed.result_json);
            Check(At(answer, {"consent_of_filmed_person"}) == "true" &&
                      mediaOf(answer).generation.kind == "dubbing" &&
                      mediaOf(answer).generation.source_media_id == source.id,
                  "the answer records the consent, the media its source");
            Check(At(submitted(), {"video_url"}) ==
                          "https://v3.fal.media/files/source.png" &&
                      At(submitted(), {"target_lang"}) == "en",
                  "the model reads the uploaded excerpt and the language");
            Check(Fake().requests.size() >= 2 &&
                      At(Parse(Fake().requests[0].body), {"content_type"}) ==
                          "video/mp4" &&
                      Fake().requests[1].body.size() > 8 &&
                      Fake().requests[1].body.compare(4, 4, "ftyp") == 0,
                  "the excerpt goes up as an MP4");
        }
        Check(!propose(R"({"kind":"dubbing","media_id":")" + source.id +
                       R"(","target_language":"anglais"})")
                   .ok,
              "a language that is no two-letter code is refused");
        Check(!propose(R"({"kind":"dubbing","media_id":")" + source.id +
                       R"(","target_language":"en","source_time":)"
                       R"({"value":3,"rate":1}})")
                   .ok,
              "a stretch that starts past the media is refused");

        // ---- lips moved to another sound ----
        const auto sync =
            propose(R"({"kind":"lipsync","media_id":")" + source.id +
                    R"(","audio_media_id":")" + source.id +
                    R"(","duration":{"value":25,"rate":25},)"
                    R"("audio_duration":{"value":25,"rate":25}})");
        Check(sync.ok, "lipsync is proposed: " + sync.message);
        const Value syncView = sync.ok ? Parse(sync.result_json) : Value();
        Check(At(syncView, {"needs_consent"}) == "true" &&
                  At(syncView, {"uploads", "0", "what"}) == "excerpt" &&
                  At(syncView, {"uploads", "1", "what"}) == "sound" &&
                  At(syncView, {"uploads", "1", "field"}) == "audio_url" &&
                  At(syncView, {"request", "sync_mode"}) == "cut_off",
              "a picture and a sound, one second each: " + sync.result_json);
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
        Fake().video = rushBytes;
        const auto synced = generate(At(syncView, {"proposal_id"}), true);
        Check(synced.ok && submitted().Find("video_url") != nullptr &&
                  submitted().Find("audio_url") != nullptr &&
                  At(submitted(), {"model"}) == "lipsync-2",
              "lipsync sends both files: " + synced.error_name + " " +
                  synced.message);
        bool wav = false;
        for (const auto& request : Fake().requests)
            wav = wav || request.body.rfind("RIFF", 0) == 0;
        Check(wav, "the sound goes up as a WAV");

        // ---- a synthetic presenter ----
        const auto filmed =
            propose(R"({"kind":"avatar","media_id":")" + source.id +
                    R"(","source_time":{"value":0,"rate":25},)"
                    R"("audio_media_id":")" +
                    source.id + R"("})");
        Check(filmed.ok &&
                  At(Parse(filmed.result_json), {"needs_consent"}) == "true" &&
                  At(Parse(filmed.result_json), {"uploads", "0", "what"}) ==
                      "frame",
              "an avatar from a frame of a rush needs the consent: " +
                  filmed.message);
        if (!generatedPicture.empty()) {
            const auto synthetic =
                propose(R"({"kind":"avatar","media_id":")" + generatedPicture +
                        R"(","audio_media_id":")" + source.id + R"("})");
            Check(synthetic.ok &&
                      At(Parse(synthetic.result_json), {"needs_consent"}) ==
                          "false" &&
                      At(Parse(synthetic.result_json),
                         {"uploads", "0", "what"}) == "still" &&
                      At(Parse(synthetic.result_json), {"model"}) ==
                          "fal-ai/kling-video/ai-avatar/v2/standard",
                  "an avatar from a picture the engine generated does not: " +
                      synthetic.message);
            Fake() = FakeFal{};
            Fake().result_body =
                R"({"video":{"url":"https://v3.fal.media/files/made.mp4"},"duration":2})";
            Fake().video = rushBytes;
            const auto spoken = generate(
                At(Parse(synthetic.result_json), {"proposal_id"}), false);
            Check(spoken.ok && submitted().Find("image_url") != nullptr &&
                      submitted().Find("audio_url") != nullptr,
                  "and runs on the person's yes alone: " + spoken.error_name +
                      " " + spoken.message);
        }
        Check(!propose(R"({"kind":"eye_contact","media_id":")" + source.id +
                       R"("})")
                   .ok,
              "no kind corrects a gaze");

        generation::SetGenerationTransport(nullptr);
        unsetenv("FAL_KEY");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    });

    // PA14 -- docs/roadmap/PARITE-2026-11.md. A shot edited by words and
    // reference pictures (fal.ai's Kling 4.0 edit), through the catalog,
    // against the fake fal.ai: what the proposal says and will cost, the
    // model's bounds said before anything leaves, the consent, the film.
    Test("PA14: edit_video, proposed then generated", [] {
        Check(generation::EditVideoMilliDollars(5, "1080p") == 1410 &&
                  generation::EditVideoMilliDollars(5, "720p") == 940 &&
                  generation::EditVideoMilliDollars(5, "480p") == 0,
              "the published rate, on the input plus the output");
        const fs::path root = fs::temp_directory_path() /
                              ("cutmachine-edit-video-" + GenerateUlid());
        fs::create_directories(root);
        std::string path;
        std::string detail;
        Check(CreatePortableProject((root / "film").string(), Project("Film"),
                                    path, detail),
              "project: " + detail);
        const fs::path rush = root / "rush.mp4";     // 1280x720, 4 s
        const fs::path small = root / "small.mp4";   // 64x36, 5 s
        const fs::path lengthy = root / "long.mp4";  // 64x36, 32 s
        Check(MakeRush(rush, 1280, 720, 4, detail), "the rush: " + detail);
        Check(MakeRush(small, 64, 36, 5, detail), "a small rush: " + detail);
        Check(MakeRush(lengthy, 64, 36, 32, detail), "a long rush: " + detail);
        const fs::path still = root / "look.png";
        const fs::path other = root / "place.png";
        {
            std::ofstream out(still, std::ios::binary);
            out << Png(64, 36);
            std::ofstream second(other, std::ios::binary);
            second << Png(48, 48);
        }
        McpProjectBackend backend(path);
        McpToolRegistry registry;
        std::string message;
        const auto ingest = [&](const fs::path& file) {
            Value args = Value::MakeObject();
            args.Set("path", Value::MakeString(file.string()));
            Check(registry.Call(backend, "ingest_media", args).ok,
                  "ingested: " + file.filename().string());
            Document now;
            backend.SnapshotDocument(now, message);
            for (const LibraryMedia& media : now.library)
                if (media.filename == file.filename().string()) return media.id;
            return Ulid();
        };
        const Ulid rushId = ingest(rush);
        const Ulid smallId = ingest(small);
        const Ulid longId = ingest(lengthy);
        const Ulid stillId = ingest(still);
        const Ulid otherId = ingest(other);
        if (rushId.empty() || smallId.empty() || longId.empty() ||
            stillId.empty() || otherId.empty()) {
            Check(false, "five media in the library");
            return;
        }
        std::string rushBytes;
        {
            std::ifstream in(rush, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            rushBytes = buffer.str();
        }

        generation::SetGenerationTransport(&FakeHttp);
        generation::SetGenerationKeyReader(nullptr);
        setenv("FAL_KEY", kKey, 1);
        const auto propose = [&](const std::string& json) {
            return registry.Call(backend, "propose_generation", Parse(json));
        };
        const auto generate = [&](const std::string& id, bool consent) {
            Value args = Value::MakeObject();
            args.Set("proposal_id", Value::MakeString(id));
            args.Set("confirmed_by_person", Value::MakeBool(true));
            if (consent)
                args.Set("consent_of_filmed_person", Value::MakeBool(true));
            return registry.Call(backend, "generate_media", args);
        };
        const auto mediaOf = [&](const Value& answer) {
            Document now;
            backend.SnapshotDocument(now, message);
            const LibraryMedia* made =
                now.FindLibraryMedia(At(answer, {"media_id"}));
            return made != nullptr ? *made : LibraryMedia{};
        };
        const std::string words =
            R"("prompt":"Replace the person in @Video1 with @Image1")";
        const std::string base =
            R"({"kind":"edit_video","media_id":")" + rushId + R"(",)" + words;

        // ---- what the proposal says ----
        const auto edit = propose(base + R"(,"reference_media_ids":[")" +
                                  stillId + R"(",")" + otherId + R"("]})");
        Check(edit.ok, "edit_video is proposed: " + edit.message);
        const Value view = edit.ok ? Parse(edit.result_json) : Value();
        Check(At(view, {"service"}) == "fal.ai" &&
                  At(view, {"model"}) == "kling/v4/video-to-video/edit" &&
                  At(view, {"spends"}) == "false" &&
                  At(view, {"needs_consent"}) == "true" &&
                  At(view, {"request", "resolution"}) == "1080p" &&
                  At(view, {"request", "prompt"}) ==
                      "Replace the person in @Video1 with @Image1" &&
                  !At(view, {"request", "image_urls", "1"}).empty() &&
                  At(view, {"request", "image_urls", "2"}).empty() &&
                  At(view, {"uploads", "0", "what"}) == "excerpt" &&
                  At(view, {"uploads", "0", "field"}) == "video_url" &&
                  At(view, {"uploads", "1", "what"}) == "still" &&
                  At(view, {"uploads", "2", "field"}) == "image_urls",
              "the model, the request, its files, the consent: " +
                  edit.result_json);
        Check(At(view, {"announced_cost"}).find("0,141") != std::string::npos &&
                  At(view, {"estimated_cost"}).find("1,13 $") !=
                      std::string::npos &&
                  At(view, {"estimated_cost"}).find("4 s d'entrée") !=
                      std::string::npos,
              "the rate, and what four seconds in and four out come to: " +
                  At(view, {"estimated_cost"}));
        const auto cheaper =
            propose(base + R"(,"resolution":"720p",)"
                           R"("duration":{"value":7,"rate":2}})");
        Check(cheaper.ok &&
                  At(Parse(cheaper.result_json), {"estimated_cost"})
                          .find("0,75 $") != std::string::npos &&
                  Parse(cheaper.result_json)
                          .Find("request")
                          ->Find("image_urls") == nullptr,
              "three and a half seconds are billed as four, in 720p, and no "
              "picture is needed: " +
                  cheaper.result_json);

        // ---- the model's bounds, said before anything leaves ----
        const auto refused = [&](const std::string& json,
                                 const std::string& expected,
                                 const std::string& what) {
            const auto answer = propose(json);
            Check(!answer.ok &&
                      answer.message.find(expected) != std::string::npos,
                  what + ": " + answer.message);
        };
        refused(R"({"kind":"edit_video","media_id":")" + rushId + R"("})",
                "says what changes", "no prompt");
        refused(base + R"(,"duration":{"value":2,"rate":1}})", "3 to 30",
                "a stretch under three seconds");
        refused(R"({"kind":"edit_video","media_id":")" + longId + R"(",)" +
                    words + "}",
                "3 to 30", "a stretch over thirty seconds");
        refused(R"({"kind":"edit_video","media_id":")" + smallId + R"(",)" +
                    words + "}",
                "700", "a picture under 700 pixels");
        refused(R"({"kind":"edit_video","media_id":")" + stillId + R"(",)" +
                    words + "}",
                "not a video", "a still as the shot");
        refused(base + R"(,"reference_media_ids":[")" + smallId + R"("]})",
                "not a still", "a video as a reference");
        refused(base + R"(,"reference_media_ids":[")" + stillId + R"(",")" +
                    stillId + R"("]})",
                "listed twice", "the same picture twice");
        std::string five;
        for (int index = 0; index < 5; ++index)
            five += std::string(index == 0 ? "" : ",") + "\"" + stillId + "\"";
        refused(base + R"(,"reference_media_ids":[)" + five + "]}", "at most 4",
                "five reference pictures");
        refused(base + R"(,"resolution":"480p"})", "1080p",
                "a definition the model does not make");
        refused(base + R"(,"target_language":"en"})",
                "is not read by 'edit_video'", "an argument of another kind");

        // ---- nothing leaves without the consent ----
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
        Fake().video = rushBytes;
        const std::string id = At(view, {"proposal_id"});
        const auto noConsent = generate(id, false);
        Check(!noConsent.ok && noConsent.error_name == "ConsentRequired" &&
                  Fake().requests.empty(),
              "without the filmed person's consent nothing leaves: " +
                  noConsent.message);
        const auto made = generate(id, true);
        Check(made.ok, "with it, the same proposal runs: " + made.error_name +
                           " " + made.message);
        if (made.ok) {
            const Value answer = Parse(made.result_json);
            const LibraryMedia media = mediaOf(answer);
            Check(
                At(answer, {"consent_of_filmed_person"}) == "true" &&
                    media.has_video && media.generation.kind == "edit_video" &&
                    media.generation.model == "kling/v4/video-to-video/edit" &&
                    media.generation.source_media_id == rushId &&
                    media.generation.prompt ==
                        "Replace the person in @Video1 with @Image1",
                "a film in the library, marked, the consent recorded");
            Value sent = Value::MakeObject();
            int puts = 0;
            bool mp4 = false;
            bool png = false;
            for (const auto& request : Fake().requests) {
                if (request.method == "POST" &&
                    request.url ==
                        "https://queue.fal.run/kling/v4/video-to-video/edit")
                    sent = Parse(request.body);
                Check(request.body.find(kKey) == std::string::npos,
                      "no body carries the key");
                if (request.method != "PUT") continue;
                ++puts;
                mp4 = mp4 || (request.body.size() > 8 &&
                              request.body.compare(4, 4, "ftyp") == 0);
                png = png || request.body.rfind("\x89PNG", 0) == 0;
            }
            const Value* pictures = sent.Find("image_urls");
            Check(!At(sent, {"video_url"}).empty() && pictures != nullptr &&
                      pictures->IsArray() && pictures->AsArray().size() == 2 &&
                      At(sent, {"resolution"}) == "1080p" &&
                      At(sent, {"prompt"}) ==
                          "Replace the person in @Video1 with @Image1",
                  "the model reads the excerpt, two pictures in an array, "
                  "the definition and the prompt: " +
                      sent.Dump());
            Check(puts == 3 && mp4 && png,
                  "the excerpt goes up as an MP4, each still as it is");
            bool polled = false;
            for (const auto& request : Fake().requests)
                polled = polled ||
                         request.url ==
                             "https://queue.fal.run/kling/v4/requests/REQ1/"
                             "status";
            Check(polled,
                  "the queue is read under the model's owner and "
                  "alias");
        }

        // ---- an HLG shot goes up as its Rec.709 picture ----
        // Kling 4.0 edit answered "Failed to load video" to a shot tagged
        // HLG, twice, on 13 November 2026, and took the same shot tagged
        // Rec.709.
        const fs::path hlg = root / "hlg.mp4";
        {
            process::Request request;
            request.arguments = {
                "",
                "-nostdin",
                "-v",
                "error",
                "-i",
                rush.string(),
                "-vf",
                "setparams=color_primaries=bt2020:color_trc=arib-std-b67:"
                "colorspace=bt2020nc",
                "-color_primaries",
                "bt2020",
                "-color_trc",
                "arib-std-b67",
                "-colorspace",
                "bt2020nc",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-c:a",
                "copy",
                "-y",
                hlg.string()};
            request.standard_output = process::Stream::kDiscard;
            request.standard_error = process::Stream::kPipe;
            request.discard_input = true;
            const process::Result tagged = ffmpeg::Run(std::move(request));
            Check(tagged.Started() && tagged.Succeeded(),
                  "an HLG rush: " + tagged.error_tail);
        }
        const Ulid hlgId = ingest(hlg);
        {
            Document now;
            backend.SnapshotDocument(now, message);
            const LibraryMedia* media = now.FindLibraryMedia(hlgId);
            Check(media != nullptr && media->color_input.transfer == "hlg",
                  "ingest reads it as HLG: " +
                      (media != nullptr ? media->color_input.transfer
                                        : std::string("no media")));
        }
        const auto bright = propose(R"({"kind":"edit_video","media_id":")" +
                                    hlgId + R"(",)" + words + "}");
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
        Fake().video = rushBytes;
        const auto brightMade =
            generate(At(Parse(bright.result_json), {"proposal_id"}), true);
        Check(brightMade.ok, "an HLG shot is edited: " + brightMade.error_name +
                                 " " + brightMade.message);
        const std::vector<generation::HttpRequest> hlgRequests =
            Fake().requests;
        int hlgUploads = 0;
        for (const auto& request : hlgRequests) {
            if (request.method != "PUT") continue;
            ++hlgUploads;
            // What went up, read back as the library reads any file.
            const fs::path sent = root / "sent-hlg.mp4";
            {
                std::ofstream out(sent, std::ios::binary);
                out << request.body;
            }
            const Ulid sentId = ingest(sent);
            Document now;
            backend.SnapshotDocument(now, message);
            const LibraryMedia* media = now.FindLibraryMedia(sentId);
            Check(media != nullptr && media->color_transfer == "bt709" &&
                      media->color_primaries == "bt709" &&
                      media->width == 1280 && media->has_audio,
                  "what goes up is tagged Rec.709, not HLG: " +
                      (media != nullptr ? media->color_transfer
                                        : std::string("no media")));
        }
        Check(hlgUploads == 1, "one file went up");
        {
            std::error_code listError;
            int leftovers = 0;
            for (const auto& entry : fs::directory_iterator(
                     fs::path(path).parent_path() / "generated", listError))
                leftovers += entry.path().extension() == ".cube" ? 1 : 0;
            Check(leftovers == 0, "the colour transform is not left behind");
        }

        // ---- a shot and a picture the engine generated: no one filmed ----
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
        Fake().video = rushBytes;
        const auto shot =
            propose(R"({"kind":"stock_video","prompt":"a walk"})");
        const auto shotMade =
            generate(At(Parse(shot.result_json), {"proposal_id"}), false);
        Fake() = FakeFal{};
        Fake().result_body =
            R"({"images":[{"url":"https://v3.fal.media/files/made.png"}]})";
        Fake().picture = Png(64, 36);
        const auto look =
            propose(R"({"kind":"stock_image","prompt":"a robot"})");
        const auto lookMade =
            generate(At(Parse(look.result_json), {"proposal_id"}), false);
        Check(shotMade.ok && lookMade.ok,
              "a generated shot and a generated picture: " + shotMade.message +
                  lookMade.message);
        if (shotMade.ok && lookMade.ok) {
            const std::string shotId =
                At(Parse(shotMade.result_json), {"media_id"});
            const std::string lookId =
                At(Parse(lookMade.result_json), {"media_id"});
            const std::string start = R"({"kind":"edit_video","media_id":")" +
                                      shotId + R"(",)" + words +
                                      R"(,"reference_media_ids":[")" + lookId;
            const auto synthetic = propose(start + R"("]})");
            Check(synthetic.ok && At(Parse(synthetic.result_json),
                                     {"needs_consent"}) == "false",
                  "nothing filmed, no consent asked: " + synthetic.message);
            const auto mixed = propose(start + R"(",")" + stillId + R"("]})");
            Check(mixed.ok &&
                      At(Parse(mixed.result_json), {"needs_consent"}) == "true",
                  "one picture the engine did not make, and it is asked");
            Fake() = FakeFal{};
            Fake().result_body =
                R"({"video":{"url":"https://v3.fal.media/files/made.mp4"}})";
            Fake().video = rushBytes;
            const auto edited = generate(
                At(Parse(synthetic.result_json), {"proposal_id"}), false);
            Check(edited.ok, "and it runs on the person's yes alone: " +
                                 edited.error_name + " " + edited.message);
        }

        generation::SetGenerationTransport(nullptr);
        unsetenv("FAL_KEY");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    });

    // DEC2 -- docs/roadmap/GENERATIF-2026-10.md. What generation never does
    // is said where an agent reads it before spending: the engine checks
    // none of it, so a rewrite of these descriptions must not lose it.
    Test("DEC2: the descriptions say what generation never does", [] {
        const McpToolRegistry registry;
        std::string generate;
        std::string propose;
        for (const McpTool& tool : registry.Tools()) {
            if (tool.name == "generate_media") generate = tool.description;
            if (tool.name == "propose_generation") propose = tool.description;
        }
        const auto says = [](const std::string& text, const char* words) {
            return text.find(words) != std::string::npos;
        };
        // DEC51 (11 November 2026) amends the first limit: a filmed
        // person is acted on only with their consent.
        Check(says(generate, "WHAT GENERATION NEVER DOES") &&
                  says(generate, "filmed person") &&
                  says(generate, "without their consent") &&
                  says(generate, "no voice, face or body") &&
                  says(generate, "'consent_of_filmed_person': true") &&
                  says(generate, "never you in their place") &&
                  says(generate, "only on a shot with no person in it") &&
                  says(generate, "framing (set_clip_framing") &&
                  says(generate, "covers paid advertising") &&
                  says(generate, "neither for Stable Audio nor for Lyria"),
              "generate_media says the three limits: " + generate);
        Check(says(propose, "propose nothing on a filmed person") &&
                  says(propose, "`needs_consent`") &&
                  says(propose, "no expansion of a shot with a person"),
              "propose_generation points to them: " + propose);
        // PA14 -- an edited shot stands under the same limit.
        Check(says(generate,
                   "edit_video on a shot or a picture the engine "
                   "did not generate") &&
                  says(propose,
                       "edit_video on a shot or a picture the "
                       "engine did not generate"),
              "and both name edit_video");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
