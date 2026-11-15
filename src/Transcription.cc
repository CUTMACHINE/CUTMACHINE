#include "Transcription.h"

#include "FfmpegBinary.h"
#include "LocalEnv.h"
#include "whisper.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

// Decodes `inputPath`'s audio to mono 16 kHz float32 PCM -- whisper.cpp's
// required input format -- using the same FFmpeg subprocess pipeline
// Waveform.cc's GenerateAudioWaveform already uses for its own decode.
bool DecodePcm16k(const std::string& ffmpegPath,
                  const std::vector<std::string>& ffmpegArguments,
                  MediaTaskContext& context, std::vector<float>& samples,
                  std::string& error) {
    std::vector<std::string> storage;
    storage.reserve(ffmpegArguments.size() + 1);
    storage.push_back(ffmpegPath);
    storage.insert(storage.end(), ffmpegArguments.begin(),
                   ffmpegArguments.end());

    std::vector<char> pendingBytes;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 8192;
    request.error_tail_bytes = 8192;
    request.cancelled = [&context]() { return context.Cancelled(); };
    request.on_output = [&](const char* bytes, size_t count) {
        pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
        size_t offset = 0;
        while (pendingBytes.size() - offset >= sizeof(float)) {
            float sample = 0.0f;
            std::memcpy(&sample, pendingBytes.data() + offset, sizeof(sample));
            offset += sizeof(sample);
            samples.push_back(sample);
        }
        pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + offset);
        context.SetProgress(0.0, "Décodage audio");
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "transcription process pipes");
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "transcription cancelled";
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty()
                    ? "FFmpeg audio decode for transcription failed"
                    : result.error_tail;
        return false;
    }
    if (samples.empty()) {
        error = "audio stream produced no samples to transcribe";
        return false;
    }
    return true;
}

bool DecodeMonoPcm16k(const std::string& inputPath,
                      const std::string& ffmpegPath, MediaTaskContext& context,
                      std::vector<float>& samples, std::string& error) {
    return DecodePcm16k(
        ffmpegPath,
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-i", inputPath,
         "-map", "0:a:0", "-vn", "-ac", "1", "-ar",
         std::to_string(WHISPER_SAMPLE_RATE), "-f", "f32le", "pipe:1"},
        context, samples, error);
}

// Groups whisper.cpp's per-token output into words and rounds each word's
// boundary onto the source's exact frame grid (Transcription.h's single
// explicit rounding rule). Whisper's tokenizer marks the start of a new word
// with a leading space in the decoded token text (its own convention, not
// ours); a token without one is treated as a continuation of the current
// word -- this is how trailing punctuation ends up attached to the word
// before it, which is the intended behavior for cutting whole words cleanly.
// Special/timestamp tokens (id >= whisper_token_eot) carry no real text and
// are skipped, mirroring whisper.cpp's own examples/cli/cli.cpp filter.
// SUBTITLE-2026-09 -- whisper.cpp offers two token timestamps and they are not
// of the same quality. `token_timestamps` reads the probability of the
// timestamp token the decoder emits after each sub-word; Whisper was never
// trained to emit one after every word, so it emits them every few words and
// the rest is interpolation. The DTW timestamps come from the cross-attention
// alignment -- the method WhisperX and stable-ts use -- and they are the ones
// worth having.
//
// Measured on the Nina montage of PRJB, against DaVinci Resolve's own
// word-by-word transcription of the same assembled audio (143 words matched):
// the heuristic start of a word sits 4 images early at the median, 57 % of
// words are off by more than 3. It never shows on a cut; it shows on every
// subtitle.
//
// The alignment heads are model-specific and whisper.cpp refuses to load a
// mismatched pair, so an unrecognised model keeps the old behaviour rather
// than guessing a preset.
whisper_alignment_heads_preset AlignmentHeadsForModel(const std::string& path) {
    std::string name = std::filesystem::path(path).filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto has = [&](const char* needle) {
        return name.find(needle) != std::string::npos;
    };
    // Ordered longest-match first: "large-v3-turbo" also contains "large-v3".
    if (has("large-v3-turbo") || has("large-v3_turbo"))
        return WHISPER_AHEADS_LARGE_V3_TURBO;
    if (has("large-v3")) return WHISPER_AHEADS_LARGE_V3;
    if (has("large-v2")) return WHISPER_AHEADS_LARGE_V2;
    if (has("large-v1")) return WHISPER_AHEADS_LARGE_V1;
    if (has("medium.en")) return WHISPER_AHEADS_MEDIUM_EN;
    if (has("medium")) return WHISPER_AHEADS_MEDIUM;
    if (has("small.en")) return WHISPER_AHEADS_SMALL_EN;
    if (has("small")) return WHISPER_AHEADS_SMALL;
    if (has("base.en")) return WHISPER_AHEADS_BASE_EN;
    if (has("base")) return WHISPER_AHEADS_BASE;
    if (has("tiny.en")) return WHISPER_AHEADS_TINY_EN;
    if (has("tiny")) return WHISPER_AHEADS_TINY;
    return WHISPER_AHEADS_NONE;
}

// Loads the model with cross-attention alignment when the heads are known,
// and without it otherwise. Default decoding may fall back to heuristic
// timestamps; the pinned AL4 profile requires the heads it was evaluated with.
whisper_context* OpenWhisperModel(const std::string& path,
                                  WhisperDecodingProfile profile) {
    const whisper_alignment_heads_preset preset =
        profile == WhisperDecodingProfile::FrenchHesitationsV1
            ? WHISPER_AHEADS_LARGE_V3
            : AlignmentHeadsForModel(path);
    if (preset != WHISPER_AHEADS_NONE) {
        whisper_context_params params = whisper_context_default_params();
        params.dtw_token_timestamps = true;
        params.dtw_aheads_preset = preset;
        if (whisper_context* ctx =
                whisper_init_from_file_with_params(path.c_str(), params))
            return ctx;
        // AL4 -- the pinned profile was evaluated with these alignment heads.
        if (profile != WhisperDecodingProfile::Default) return nullptr;
    }
    whisper_context_params params = whisper_context_default_params();
    return whisper_init_from_file_with_params(path.c_str(), params);
}

std::vector<TranscriptWord> GroupWordsFromWhisper(whisper_context* ctx,
                                                  whisper_state* state,
                                                  const MediaRate& frameRate) {
    std::vector<TranscriptWord> words;
    const whisper_token eot = whisper_token_eot(ctx);
    const int segmentCount = whisper_full_n_segments_from_state(state);
    std::string currentText;
    int64_t currentStartCentis = 0;
    int64_t currentEndCentis = 0;
    // -1 is whisper.cpp's own sentinel for "no DTW timestamp on this token"
    // (see the whisper_token_data initialiser); it is what a model loaded
    // without alignment heads leaves behind, so it also selects the fallback.
    int64_t currentStartDtw = -1;
    const auto flush = [&]() {
        if (currentText.empty()) return;
        std::string text = currentText;
        if (!text.empty() && text.front() == ' ') text.erase(text.begin());
        if (!text.empty()) {
            // The DTW timestamp is one instant per token -- the moment the
            // token was emitted -- not a span, so it can only place the word,
            // not measure it. Keeping whisper's own duration and moving the
            // pair together corrects what is wrong (where the word sits)
            // without inventing what is not measured (how long it lasts).
            //
            // ITW9 -- except for an ellipsis: "..." is not a sound but the
            // span whisper skipped, and the instant it was emitted is that
            // span's end. Moved there with its duration, one ellipsis over a
            // 19 s take was cached from 19.6 to 39.4 s, past the end of the
            // media and over none of its speech (15 November 2026).
            int64_t startCentis = currentStartCentis;
            int64_t endCentis = currentEndCentis;
            if (currentStartDtw >= 0 && !IsEllipsisOnlyWord(text)) {
                endCentis =
                    currentStartDtw + (currentEndCentis - currentStartCentis);
                startCentis = currentStartDtw;
            }
            const RationalTime start{startCentis, 100};
            const RationalTime end{endCentis, 100};
            words.push_back({text, RoundToSourceFrame(start, frameRate, false),
                             RoundToSourceFrame(end, frameRate, true)});
        }
        currentText.clear();
        currentStartDtw = -1;
    };
    for (int segment = 0; segment < segmentCount; ++segment) {
        const int tokenCount = whisper_full_n_tokens_from_state(state, segment);
        for (int token = 0; token < tokenCount; ++token) {
            const whisper_token_data data =
                whisper_full_get_token_data_from_state(state, segment, token);
            if (data.id >= eot) continue;
            const char* text = whisper_full_get_token_text_from_state(
                ctx, state, segment, token);
            if (!text || !*text) continue;
            const bool startsNewWord = currentText.empty() || text[0] == ' ';
            if (startsNewWord && !currentText.empty()) flush();
            if (currentText.empty()) {
                currentStartCentis = data.t0;
                currentStartDtw = data.t_dtw;
            }
            currentText += text;
            currentEndCentis = data.t1;
        }
    }
    flush();
    return words;
}

bool AppendWordText(std::string& destination, const std::string& text) {
    if (text.empty()) return false;
    constexpr std::string_view kPunctuation = ".,!?;:%)]}";
    if (!destination.empty() &&
        kPunctuation.find(text.front()) == std::string_view::npos)
        destination.push_back(' ');
    destination += text;
    return true;
}

// Outward frame rounding can make neighboring words share one source frame,
// and Whisper occasionally emits a zero-centisecond token. Keep the text in
// order while assigning only representable, non-overlapping frame spans.
// Words too short to own a frame join the next representable word (or the
// preceding one at EOF); this is preferable to caching an artifact our exact
// time model must reject later.
// Where the words being normalized come from. whisper.cpp's token
// timestamps are only *locally* monotonic: a temperature fallback re-decodes
// a window and can restart it slightly before the previous window's last
// token. That is documented decoder behaviour, not corruption, so it is
// absorbed at the boundary with whisper.cpp. A stored cache is held to the
// stricter rule -- there, out of order can only mean a damaged or
// hand-edited sidecar, and silently repairing it would hide the damage.
enum class WordSource { Decoder, StoredCache };

bool NormalizeTranscriptWords(std::vector<TranscriptWord>& words,
                              std::string& error,
                              WordSource source = WordSource::StoredCache) {
    std::vector<TranscriptWord> normalized;
    normalized.reserve(words.size());
    std::string pendingText;
    RationalTime previousRawStart{-1, 1};
    bool first = true;
    for (TranscriptWord& word : words) {
        if (word.text.empty()) {
            error = "transcript word is empty";
            return false;
        }
        if (!first && word.start < previousRawStart) {
            if (source == WordSource::StoredCache) {
                error = "transcript words are not in chronological order";
                return false;
            }
            word.start = previousRawStart;
            if (word.end < word.start) word.end = word.start;
        }
        previousRawStart = word.start;
        first = false;
        if (!normalized.empty() && word.start < normalized.back().end)
            word.start = normalized.back().end;
        if (word.end <= word.start) {
            AppendWordText(pendingText, word.text);
            continue;
        }
        if (!pendingText.empty()) {
            AppendWordText(pendingText, word.text);
            word.text = std::move(pendingText);
            pendingText.clear();
        }
        normalized.push_back(std::move(word));
    }
    if (!pendingText.empty()) {
        if (normalized.empty()) {
            error = "transcript contains no frame-representable words";
            return false;
        }
        AppendWordText(normalized.back().text, pendingText);
    }
    words = std::move(normalized);
    return true;
}

void WriteJsonString(std::ostringstream& output, const std::string& value) {
    output << '"';
    for (const char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
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
                if (static_cast<unsigned char>(character) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  character & 0xff);
                    output << buffer;
                } else {
                    output << character;
                }
        }
    }
    output << '"';
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

// One whisper window. Verbatim decoding re-enters whisper.cpp once per
// window so the filler prompt sits in front of each of them.
constexpr int kVerbatimWindowMs = 30000;

constexpr char kVerbatimPrompt[] =
    "Euh, heu, hum, mmh, eh bien, enfin, donc... je, je veux dire... ";

// ITW9 -- whisper.cpp matches suppress_regex against each vocabulary token
// whole (std::regex_match). Two dots or more, or ellipsis characters, with
// their spaces: never the single "." that ends a sentence.
constexpr char kEllipsisOnlyTokenPattern[] = "[ ]*(\\.\\.+|(…)+)[ ]*";

bool SaveTranscript(const std::filesystem::path& destination,
                    const Transcript& transcript, std::string& error) {
    if (!ValidateWhisperDecodingRequest(transcript.decoding_profile,
                                        transcript.language,
                                        transcript.verbatim, error))
        return false;
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create transcript directory: " +
                filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.filename().string() + ".partial-" + GenerateUlid());
    std::ostringstream output;
    output << "{\"version\":1,\"media_id\":";
    WriteJsonString(output, transcript.media_id);
    output << ",\"whisper_model\":";
    WriteJsonString(output, transcript.whisper_model);
    output << ",\"language\":";
    WriteJsonString(output, transcript.language);
    // ITW9 -- written only when known, so a transcript made with its
    // language forced, or before detection was recorded, keeps its bytes.
    if (!transcript.detected_language.empty()) {
        output << ",\"detected_language\":";
        WriteJsonString(output, transcript.detected_language);
    }
    output << ",\"verbatim\":" << (transcript.verbatim ? "true" : "false");
    if (transcript.decoding_profile != WhisperDecodingProfile::Default) {
        output << ",\"decoding_profile\":";
        WriteJsonString(
            output, WhisperDecodingProfileName(transcript.decoding_profile));
    }
    // ITW9 -- written only when a plain pass completed a verbatim one, so
    // every other transcript keeps its bytes.
    if (transcript.plain_pass_words > 0) {
        output << ",\"plain_pass_words\":" << transcript.plain_pass_words;
    }
    // ITW9 -- likewise, only when words took the place of ellipses.
    if (transcript.ellipsis_pass_words > 0) {
        output << ",\"ellipsis_pass_words\":" << transcript.ellipsis_pass_words;
    }
    // Written only when set, so a transcript that has never been through the
    // alignment pass keeps the exact bytes it had before this field existed.
    if (transcript.speech_aligned) output << ",\"speech_aligned\":true";
    if (transcript.speech_assessed) {
        output << ",\"speech_assessed\":true,\"measured_speech_duration\":";
        WriteTime(output, transcript.measured_speech_duration);
        output << ",\"likely_hallucinated\":"
               << (transcript.likely_hallucinated ? "true" : "false")
               << ",\"likely_incomplete\":"
               << (transcript.likely_incomplete ? "true" : "false")
               << ",\"known_hallucination_phrase\":"
               << (transcript.known_hallucination_phrase ? "true" : "false");
    }
    output << ",\"source_rate\":{\"num\":" << transcript.source_rate.num
           << ",\"den\":" << transcript.source_rate.den << "},\"words\":[";
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        if (index) output << ',';
        const TranscriptWord& word = transcript.words[index];
        output << "{\"text\":";
        WriteJsonString(output, word.text);
        output << ",\"start\":";
        WriteTime(output, word.start);
        output << ",\"end\":";
        WriteTime(output, word.end);
        output << '}';
    }
    output << "]}\n";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << output.str();
    file.close();
    if (!file) {
        error = "unable to write transcript cache";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error =
            "unable to install transcript cache: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

// Minimal hand-rolled JSON reader for the transcript cache, matching the
// style Operations.cc's Reader and EditLog.cc's LogReader already use in
// this codebase rather than sharing a parser across translation units
// (neither of those is exposed for reuse outside its own file).
class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character == '\\') {
                if (position_ >= input_.size())
                    throw std::runtime_error("unterminated string escape");
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"':
                        output.push_back('"');
                        break;
                    case '\\':
                        output.push_back('\\');
                        break;
                    case 'n':
                        output.push_back('\n');
                        break;
                    case 'r':
                        output.push_back('\r');
                        break;
                    case 't':
                        output.push_back('\t');
                        break;
                    case '/':
                        output.push_back('/');
                        break;
                    case 'b':
                        output.push_back('\b');
                        break;
                    case 'f':
                        output.push_back('\f');
                        break;
                    case 'u':
                        AppendUtf8(UnicodeEscape(), output);
                        break;
                    default:
                        throw std::runtime_error("unsupported string escape");
                }
            } else {
                output.push_back(character);
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        return static_cast<int64_t>(value);
    }

    bool Boolean() {
        if (Consume("true")) return true;
        if (Consume("false")) return false;
        throw std::runtime_error("expected boolean at byte " +
                                 std::to_string(position_));
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing transcript JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    // The four hex digits after `\u`, refused when one is not a hex digit:
    // strtol used to stop there and read the rest as zero.
    uint32_t Hex4() {
        if (position_ + 4 > input_.size()) {
            throw std::runtime_error("truncated unicode escape");
        }
        uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<uint32_t>(digit - 'A' + 10);
            } else {
                throw std::runtime_error("invalid unicode escape");
            }
        }
        return value;
    }

    // Transcript cache escapes -- docs/roadmap/DONNEES-2026-10.md (a native
    // defect noted during OR3). The engine writes words as UTF-8 and escapes
    // only control characters, but a cache rewritten by another tool may
    // carry every accent as `\u00e9` (Python's json.dumps does by default)
    // and a character outside the BMP as a surrogate pair. Keeping the low
    // seven bits read « Société » as « Sociiti » without an error; a lone
    // surrogate is refused rather than written as invalid UTF-8.
    uint32_t UnicodeEscape() {
        uint32_t codePoint = Hex4();
        if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            throw std::runtime_error("unpaired low surrogate escape");
        }
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
                throw std::runtime_error("unpaired high surrogate escape");
            }
            position_ += 2;
            const uint32_t low = Hex4();
            if (low < 0xDC00 || low > 0xDFFF) {
                throw std::runtime_error("invalid low surrogate escape");
            }
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
        }
        return codePoint;
    }

    static void AppendUtf8(uint32_t codePoint, std::string& output) {
        if (codePoint < 0x80) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    const std::string& input_;
    size_t position_ = 0;
};

RationalTime ReadTime(Reader& reader) {
    reader.Expect("{\"value\":");
    const int64_t value = reader.Integer();
    reader.Expect(",\"rate\":");
    const int64_t rate = reader.Integer();
    reader.Expect("}");
    if (rate <= 0 || rate > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("transcript RationalTime rate out of range");
    }
    return {value, static_cast<int32_t>(rate)};
}

}  // namespace

RationalTime RoundToSourceFrame(const RationalTime& time,
                                const MediaRate& frameRate, bool roundUp) {
    if (frameRate.num <= 0 || frameRate.den <= 0)
        throw std::invalid_argument("frame rate must be positive");
    const int64_t frameIndex = time.to_frames(frameRate.num, frameRate.den);
    const RationalTime floorTime{
        frameIndex * static_cast<int64_t>(frameRate.den), frameRate.num};
    if (!roundUp || floorTime == time) return floorTime;
    return RationalTime{(frameIndex + 1) * static_cast<int64_t>(frameRate.den),
                        frameRate.num};
}

void CountWordsInLanguage(std::vector<std::pair<std::string, size_t>>& counts,
                          const std::string& language, size_t words) {
    for (auto& [heard, count] : counts) {
        if (heard == language) {
            count += words;
            return;
        }
    }
    counts.emplace_back(language, words);
}

std::string MostSpokenLanguage(
    const std::vector<std::pair<std::string, size_t>>& counts) {
    const std::pair<std::string, size_t>* best = nullptr;
    for (const auto& entry : counts) {
        if (best == nullptr || entry.second > best->second) {
            best = &entry;
        }
    }
    return best != nullptr ? best->first : std::string();
}

namespace {

// QC-2026-09 A3 -- one media, against a model already in memory. Split out of
// GenerateAudioTranscript so a batch loads the model once: the load measured
// about 8 s, which on the 43 spoken rushes of one project was nearly six
// minutes spent doing the same thing forty-three times. Nothing else about
// the pass changed when it moved here.
//
// Sharing the context across media does not leak one rush's words into the
// next one's decoding: whisper_full_default_params sets no_context, and
// whisper.cpp clears prompt_past at the top of every whisper_full call. The
// verbatim path's per-window prompt seeding below is unaffected for the same
// reason -- it was always per call, never per context.
//
// ITW9 -- docs/roadmap/ITW-2026-09.md. What did leak is the decoding state.
// A window that fails its thresholds is decoded again at a higher
// temperature, by sampling, and whisper.cpp seeds the random generator of
// its first decoder once per state, not once per call: on one shared state,
// a rush came out with other words depending on the rushes decoded before
// it in the batch (measured on 15 November 2026: the same take, decoded
// twice in a row, lost a word the second time). Each media now gets a state
// of its own, seeded as in a fresh process, so a transcription is the same
// whatever the batch around it; the model, the costly part, is still loaded
// once.
struct WhisperStateDeleter {
    void operator()(whisper_state* state) const { whisper_free_state(state); }
};

bool TranscribeSamplesWithModel(whisper_context* ctx,
                                const std::vector<float>& samples,
                                const std::string& outputPath,
                                const std::string& mediaId,
                                const MediaRate& sourceRate,
                                const WhisperSettings& settings,
                                MediaTaskContext& context, std::string& error) {
    error.clear();
    if (sourceRate.num <= 0 || sourceRate.den <= 0) {
        error = "invalid source frame rate";
        return false;
    }
    if (samples.empty()) {
        error = "audio stream produced no samples to transcribe";
        return false;
    }
    if (context.Cancelled()) {
        error = "transcription cancelled";
        return false;
    }

    context.SetProgress(0.5, "Transcription");
    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.translate = false;
    params.token_timestamps = true;
    params.single_segment = false;
    const std::string language =
        settings.language.empty() ? "auto" : settings.language;
    params.initial_prompt = settings.verbatim ? kVerbatimPrompt : nullptr;
    // ALPHA-2026-08 -- whisper.cpp conditions every 30 s window on the text
    // decoded so far (its prompt_past, consulted only while n_max_text_ctx is
    // above zero). On a rush, speech is a minority of the runtime: one
    // caption credit hallucinated over music or room tone is fed back as
    // context and then repeats to the end of the media. Measured on a 7 min
    // 35 interview, large-v3 emitted "Sous-titrage Societe Radio-Canada" 32
    // times and not one real sentence. Cross-window coherence is worth less
    // than never poisoning the remainder of the transcript.
    //
    // The verbatim pass cannot take that shortcut: the filler prompt reaches
    // the decoder through the very same prompt_past, so it needs
    // conditioning switched on. It buys back the safety by walking the media
    // one window per call (see kVerbatimWindowMs below), which bounds any
    // hallucination to the window that produced it.
    params.n_max_text_ctx = settings.verbatim ? 16384 : 0;
    // Caption furniture (">>", musical notes, brackets) is never a spoken
    // word. Suppressing those tokens removes at the source the artifacts the
    // editorial layer would otherwise have to recognise and filter.
    params.suppress_nst = true;
    // ITW9 -- the pass the transcription command makes again over speech
    // that came out as an ellipsis (see EllipsisOverSpeech in
    // Transcription.h): whisper may no longer answer with "..." alone, so
    // it decodes the words or says something the caller can refuse.
    params.suppress_regex =
        settings.suppress_ellipsis ? kEllipsisOnlyTokenPattern : nullptr;
    params.language = language.c_str();
    params.n_threads =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    // IA5 -- ROADMAP.md. Outside verbatim mode one whisper_full call covers
    // the whole media, and the task was only consulted once it returned: a
    // cancelled transcription of a long rush kept the model busy for
    // minutes. whisper.cpp asks this between graph computations.
    params.abort_callback = [](void* data) {
        return static_cast<MediaTaskContext*>(data)->Cancelled();
    };
    params.abort_callback_user_data = &context;
    if (!ConfigureWhisperDecodingProfile(settings.decoding_profile, ctx, params,
                                         error))
        return false;

    // whisper.cpp seeds prompt_past from initial_prompt once per call, then
    // overwrites it with each decoded segment (its own "update prompt_past").
    // A single call therefore biases only the first window toward fillers --
    // measured: identical words and timecodes with and without --verbatim
    // over a 7 min media. Advancing one window per call is what makes the
    // prompt reach every window. Boundaries land on whisper's own segment
    // ends rather than on a fixed grid, so no word is cut in half.
    const int64_t totalCentis =
        static_cast<int64_t>(samples.size()) * 100 / WHISPER_SAMPLE_RATE;
    std::vector<TranscriptWord> words;
    // ITW9 -- with "auto", whisper.cpp detects the language of each call;
    // a verbatim pass makes one call per window, and may hear a crew aside
    // in another language. Each detection counts for the words it decoded.
    std::vector<std::pair<std::string, size_t>> wordsByLanguage;
    const std::unique_ptr<whisper_state, WhisperStateDeleter> state(
        whisper_init_state(ctx));
    if (!state) {
        error = "local whisper.cpp could not allocate a decoding state";
        return false;
    }
    int64_t cursorCentis = 0;
    while (true) {
        params.offset_ms = static_cast<int>(cursorCentis * 10);
        params.duration_ms = settings.verbatim ? kVerbatimWindowMs : 0;
        const int result =
            whisper_full_with_state(ctx, state.get(), params, samples.data(),
                                    static_cast<int>(samples.size()));
        if (result != 0 && !context.Cancelled()) {
            error = "local whisper.cpp inference failed with code " +
                    std::to_string(result);
            return false;
        }
        if (context.Cancelled()) {
            error = "transcription cancelled";
            return false;
        }
        std::vector<TranscriptWord> pass =
            GroupWordsFromWhisper(ctx, state.get(), sourceRate);
        if (language == "auto" && !pass.empty()) {
            const int detected = whisper_full_lang_id_from_state(state.get());
            if (const char* code =
                    detected >= 0 ? whisper_lang_str(detected) : nullptr) {
                CountWordsInLanguage(wordsByLanguage, code, pass.size());
            }
        }
        words.insert(words.end(), std::make_move_iterator(pass.begin()),
                     std::make_move_iterator(pass.end()));
        if (!settings.verbatim) break;

        const int segmentCount =
            whisper_full_n_segments_from_state(state.get());
        // Whisper stops a window on the last token it decoded, not on the
        // window's nominal end. Resuming there is what keeps a sentence
        // whole; a window that decoded nothing usable falls back to a fixed
        // step so the loop always terminates.
        const int64_t decodedEnd = segmentCount > 0
                                       ? whisper_full_get_segment_t1_from_state(
                                             state.get(), segmentCount - 1)
                                       : 0;
        cursorCentis = decodedEnd > cursorCentis
                           ? decodedEnd
                           : cursorCentis + kVerbatimWindowMs / 10;
        if (cursorCentis + 100 >= totalCentis) break;
        if (totalCentis > 0) {
            context.SetProgress(0.5 + 0.5 * static_cast<double>(cursorCentis) /
                                          static_cast<double>(totalCentis),
                                "Transcription");
        }
    }

    Transcript transcript;
    transcript.media_id = mediaId;
    transcript.whisper_model =
        std::filesystem::path(settings.whisper_model_path).filename().string();
    transcript.language = language;
    transcript.detected_language = MostSpokenLanguage(wordsByLanguage);
    transcript.verbatim = settings.verbatim;
    transcript.decoding_profile = settings.decoding_profile;
    transcript.source_rate = sourceRate;
    transcript.words = std::move(words);

    if (transcript.words.empty()) {
        error = "transcription produced no words";
        return false;
    }
    if (!NormalizeTranscriptWords(transcript.words, error, WordSource::Decoder))
        return false;
    context.SetProgress(1.0, "Transcript prêt");
    return SaveTranscript(outputPath, transcript, error);
}

}  // namespace

bool GenerateAudioTranscripts(const std::vector<TranscriptionJob>& jobs,
                              const WhisperSettings& settings,
                              MediaTaskContext& context,
                              std::vector<TranscriptionOutcome>& outcomes,
                              std::string& error) {
    error.clear();
    outcomes.clear();
    if (jobs.empty()) {
        error = "no media to transcribe";
        return false;
    }
    if (settings.whisper_model_path.empty()) {
        error = "a local whisper.cpp model path is required";
        return false;
    }
    if (!ValidateWhisperDecodingRequest(settings.decoding_profile,
                                        settings.language, settings.verbatim,
                                        error) ||
        !ValidateWhisperDecodingModel(settings.decoding_profile,
                                      settings.whisper_model_path, context,
                                      error))
        return false;
    whisper_context* ctx = OpenWhisperModel(settings.whisper_model_path,
                                            settings.decoding_profile);
    if (!ctx) {
        error = "unable to load local whisper.cpp model '" +
                settings.whisper_model_path + "'";
        return false;
    }
    // One media's failure is not the batch's: a rush that will not decode
    // should not cost the model load for the forty that would have. Each
    // outcome carries its own reason, and the caller decides what to do
    // with a partial result.
    for (const TranscriptionJob& job : jobs) {
        TranscriptionOutcome outcome;
        outcome.media_id = job.media_id;
        std::string jobError;
        std::vector<float> samples;
        outcome.ok = DecodeMonoPcm16k(job.input_path, settings.ffmpeg_path,
                                      context, samples, jobError) &&
                     TranscribeSamplesWithModel(ctx, samples, job.output_path,
                                                job.media_id, job.source_rate,
                                                settings, context, jobError);
        outcome.error = outcome.ok ? std::string() : jobError;
        if (outcome.ok) {
            Transcript written;
            std::string readError;
            if (LoadAudioTranscript(job.output_path, written, readError))
                outcome.words = written.words.size();
        }
        outcomes.push_back(std::move(outcome));
        if (context.Cancelled()) break;
    }
    whisper_free(ctx);
    return true;
}

bool DecodeFfmpegAudioToPcm16k(const std::string& ffmpegPath,
                               const std::vector<std::string>& ffmpegArguments,
                               MediaTaskContext& context,
                               std::vector<float>& samples,
                               std::string& error) {
    return DecodePcm16k(ffmpegPath, ffmpegArguments, context, samples, error);
}

bool GenerateAudioTranscriptFromPcm(const std::vector<float>& samples,
                                    const std::string& outputPath,
                                    const std::string& mediaId,
                                    const MediaRate& sourceRate,
                                    const WhisperSettings& settings,
                                    MediaTaskContext& context,
                                    std::string& error) {
    error.clear();
    if (settings.whisper_model_path.empty()) {
        error = "a local whisper.cpp model path is required";
        return false;
    }
    if (!ValidateWhisperDecodingRequest(settings.decoding_profile,
                                        settings.language, settings.verbatim,
                                        error) ||
        !ValidateWhisperDecodingModel(settings.decoding_profile,
                                      settings.whisper_model_path, context,
                                      error))
        return false;
    whisper_context* ctx = OpenWhisperModel(settings.whisper_model_path,
                                            settings.decoding_profile);
    if (!ctx) {
        error = "unable to load local whisper.cpp model '" +
                settings.whisper_model_path + "'";
        return false;
    }
    const bool result =
        TranscribeSamplesWithModel(ctx, samples, outputPath, mediaId,
                                   sourceRate, settings, context, error);
    whisper_free(ctx);
    return result;
}

bool GenerateAudioTranscript(const std::string& inputPath,
                             const std::string& outputPath,
                             const std::string& mediaId,
                             const MediaRate& sourceRate,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, std::string& error) {
    std::vector<TranscriptionOutcome> outcomes;
    if (!GenerateAudioTranscripts(
            {{mediaId, inputPath, outputPath, sourceRate}}, settings, context,
            outcomes, error))
        return false;
    if (outcomes.empty()) {
        error = "transcription produced no outcome";
        return false;
    }
    error = outcomes.front().error;
    return outcomes.front().ok;
}

bool SaveAudioTranscript(const std::string& path, const Transcript& transcript,
                         std::string& error) {
    error.clear();
    if (transcript.word_timing != "whisper") {
        error = "refusing to write " + transcript.word_timing +
                " word boundaries into the Whisper transcript cache";
        return false;
    }
    return SaveTranscript(std::filesystem::path(path), transcript, error);
}

std::string CachedTranscriptPreferredReason(const Transcript& cached,
                                            const Transcript& incoming) {
    if (cached.words.empty() || cached.likely_incomplete ||
        cached.likely_hallucinated) {
        return std::string();
    }
    const std::string counts =
        std::to_string(incoming.words.size()) + " words, against " +
        std::to_string(cached.words.size()) + " in the cached one";
    if (incoming.likely_incomplete || incoming.likely_hallucinated) {
        return std::string("the new transcription is flagged ") +
               (incoming.likely_incomplete ? "likely_incomplete"
                                           : "likely_hallucinated") +
               " and the cached one is not (" + counts + ")";
    }
    if (incoming.verbatim == cached.verbatim &&
        incoming.words.size() * 2 < cached.words.size()) {
        return "the new transcription holds fewer than half the words of "
               "the cached one, made the same way (" +
               counts + ")";
    }
    return std::string();
}

bool InstallTranscriptUnlessWorse(const std::string& cachePath,
                                  const Transcript& incoming, bool overwrite,
                                  TranscriptionOutcome& outcome,
                                  std::string& error) {
    error.clear();
    Transcript cached;
    std::string cachedError;
    if (!overwrite && LoadAudioTranscript(cachePath, cached, cachedError)) {
        const std::string reason =
            CachedTranscriptPreferredReason(cached, incoming);
        if (!reason.empty()) {
            outcome.cache_kept = true;
            outcome.cache_kept_reason = reason;
            outcome.cached_words = cached.words.size();
            return true;
        }
    }
    return SaveAudioTranscript(cachePath, incoming, error);
}

bool TranscriptCacheMatches(const Transcript& transcript,
                            const std::string& mediaId,
                            const WhisperSettings& settings) {
    const std::string model =
        std::filesystem::path(settings.whisper_model_path).filename().string();
    const std::string language =
        settings.language.empty() ? "auto" : settings.language;
    return transcript.media_id == mediaId &&
           transcript.whisper_model == model &&
           transcript.language == language &&
           transcript.verbatim == settings.verbatim &&
           transcript.decoding_profile == settings.decoding_profile;
}

bool ResolveWhisperSettings(const WhisperDecodingRequest& request,
                            const std::string& hesitationsModel,
                            MediaTaskContext& context,
                            WhisperSettings& settings,
                            WhisperDecodingChoice& choice, std::string& error) {
    if (!ResolveWhisperDecodingProfile(
            request, settings.language.empty() ? "auto" : settings.language,
            settings.verbatim, settings.whisper_model_path, hesitationsModel,
            context, choice, error)) {
        return false;
    }
    settings.decoding_profile = choice.profile;
    settings.whisper_model_path = choice.model_path;
    return true;
}

WhisperSettings PlainFallbackSettings(const WhisperSettings& configured) {
    WhisperSettings plain = configured;
    plain.verbatim = false;
    plain.decoding_profile = WhisperDecodingProfile::Default;
    return plain;
}

WhisperSettings EllipsisRedoSettings(const WhisperSettings& resolved,
                                     const WhisperSettings& configured,
                                     bool verbatim) {
    WhisperSettings redo =
        verbatim ? resolved : PlainFallbackSettings(configured);
    redo.verbatim = verbatim;
    redo.suppress_ellipsis = true;
    return redo;
}

bool LoadAudioTranscript(const std::string& path, Transcript& transcript,
                         std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open transcript cache";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read transcript cache";
        return false;
    }
    try {
        // Reader stores a reference; keep the parsed text alive in a named
        // local for the reader's whole lifetime rather than binding it to
        // buffer.str()'s temporary, which would be destroyed at the end of
        // this statement.
        const std::string json = buffer.str();
        Reader reader(json);
        reader.Expect("{\"version\":1,\"media_id\":");
        Transcript parsed;
        parsed.media_id = reader.String();
        reader.Expect(",\"whisper_model\":");
        parsed.whisper_model = reader.String();
        if (reader.Consume(",\"language\":")) parsed.language = reader.String();
        if (reader.Consume(",\"detected_language\":")) {
            parsed.detected_language = reader.String();
        }
        if (reader.Consume(",\"verbatim\":"))
            parsed.verbatim = reader.Boolean();
        if (reader.Consume(",\"decoding_profile\":")) {
            if (!ParseWhisperDecodingProfile(reader.String(),
                                             parsed.decoding_profile, error) ||
                !ValidateWhisperDecodingRequest(parsed.decoding_profile,
                                                parsed.language,
                                                parsed.verbatim, error)) {
                throw std::runtime_error(error);
            }
        }
        if (reader.Consume(",\"plain_pass_words\":")) {
            const int64_t count = reader.Integer();
            if (count <= 0 || !parsed.verbatim) {
                throw std::runtime_error(
                    "plain_pass_words is a positive count on a verbatim "
                    "transcript");
            }
            parsed.plain_pass_words = static_cast<size_t>(count);
        }
        if (reader.Consume(",\"ellipsis_pass_words\":")) {
            const int64_t count = reader.Integer();
            if (count <= 0) {
                throw std::runtime_error(
                    "ellipsis_pass_words is a positive count");
            }
            parsed.ellipsis_pass_words = static_cast<size_t>(count);
        }
        if (reader.Consume(",\"speech_aligned\":"))
            parsed.speech_aligned = reader.Boolean();
        if (reader.Consume(",\"speech_assessed\":")) {
            parsed.speech_assessed = reader.Boolean();
            reader.Expect(",\"measured_speech_duration\":");
            parsed.measured_speech_duration = ReadTime(reader);
            reader.Expect(",\"likely_hallucinated\":");
            parsed.likely_hallucinated = reader.Boolean();
            if (reader.Consume(",\"likely_incomplete\":"))
                parsed.likely_incomplete = reader.Boolean();
            reader.Expect(",\"known_hallucination_phrase\":");
            parsed.known_hallucination_phrase = reader.Boolean();
            if (!parsed.speech_assessed) {
                throw std::runtime_error(
                    "transcript speech assessment marker must be true");
            }
        }
        reader.Expect(",\"source_rate\":{\"num\":");
        const int64_t num = reader.Integer();
        reader.Expect(",\"den\":");
        const int64_t den = reader.Integer();
        reader.Expect("},\"words\":[");
        if (num <= 0 || num > std::numeric_limits<int32_t>::max() || den <= 0 ||
            den > std::numeric_limits<int32_t>::max()) {
            throw std::runtime_error("invalid transcript source_rate");
        }
        parsed.source_rate = {static_cast<int32_t>(num),
                              static_cast<int32_t>(den)};
        if (!reader.Consume("]")) {
            while (true) {
                TranscriptWord word;
                reader.Expect("{\"text\":");
                word.text = reader.String();
                reader.Expect(",\"start\":");
                word.start = ReadTime(reader);
                reader.Expect(",\"end\":");
                word.end = ReadTime(reader);
                reader.Expect("}");
                parsed.words.push_back(std::move(word));
                if (reader.Consume("]")) break;
                reader.Expect(",");
            }
        }
        reader.Expect("}");
        reader.Finish();
        if (!NormalizeTranscriptWords(parsed.words, error)) return false;
        transcript = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

namespace {

// Folds one transcript token down to the bare letters that identify it:
// lowercase, unaccented, punctuation and spacing dropped. Whisper glues
// punctuation to the word it follows ("partiel,") and now and then emits two
// words inside one token ("? Donc"), so comparing raw text would miss most
// repetitions. Only the Latin-1 accented letters French actually uses are
// folded; anything else is skipped rather than guessed at.
std::string FoldWordText(const std::string& text) {
    std::string folded;
    folded.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte >= 'A' && byte <= 'Z') {
            folded.push_back(static_cast<char>(byte - 'A' + 'a'));
            continue;
        }
        if (byte >= 'a' && byte <= 'z') {
            folded.push_back(static_cast<char>(byte));
            continue;
        }
        if (byte != 0xC3 || index + 1 >= text.size()) continue;
        const unsigned char accented =
            static_cast<unsigned char>(text[index + 1]) | 0x20;
        ++index;
        switch (accented) {
            case 0xA0:
            case 0xA1:
            case 0xA2:
            case 0xA3:
            case 0xA4:
            case 0xA5:
                folded.push_back('a');
                break;
            case 0xA7:
                folded.push_back('c');
                break;
            case 0xA8:
            case 0xA9:
            case 0xAA:
            case 0xAB:
                folded.push_back('e');
                break;
            case 0xAC:
            case 0xAD:
            case 0xAE:
            case 0xAF:
                folded.push_back('i');
                break;
            case 0xB2:
            case 0xB3:
            case 0xB4:
            case 0xB5:
            case 0xB6:
                folded.push_back('o');
                break;
            case 0xB9:
            case 0xBA:
            case 0xBB:
            case 0xBC:
                folded.push_back('u');
                break;
            default:
                break;
        }
    }
    return folded;
}

// "euuuh" and "euh" are the same hesitation held for different lengths, and
// whisper spells them both ways, so runs of one letter collapse before the
// lexicon is consulted.
std::string CollapseRepeatedLetters(const std::string& folded) {
    std::string collapsed;
    collapsed.reserve(folded.size());
    for (const char letter : folded)
        if (collapsed.empty() || collapsed.back() != letter)
            collapsed.push_back(letter);
    return collapsed;
}

// Deliberately short. Every entry here is a syllable that is not a French
// word, so removing it can never remove meaning. "eu" is absent on purpose:
// collapsing "euu" lands on it, but it is also the past participle of
// "avoir" -- the kind of false positive that would cut a real word out of a
// sentence.
bool IsFillerWord(const std::string& folded) {
    static const std::set<std::string> kFillers = {
        "euh", "heu", "hum", "hm", "mh", "ben", "bah", "beh", "hein"};
    return kFillers.count(CollapseRepeatedLetters(folded)) != 0;
}

// AGENT-2026-09 A35 -- how many words from `index` form a discourse marker,
// zero when none does. Two-word markers first, so "en fait" is not read as
// "en" alone.
size_t MarkerLength(const std::vector<std::string>& folded, size_t index) {
    static const std::set<std::string> kSingle = {"donc", "enfin", "voila",
                                                  "bref", "genre"};
    static const std::set<std::string> kPairs = {"en fait", "en gros",
                                                 "du coup", "tu vois"};
    if (index + 1 < folded.size() &&
        kPairs.count(folded[index] + " " + folded[index + 1]) != 0)
        return 2;
    return kSingle.count(folded[index]) != 0 ? 1 : 0;
}

bool ContainsKnownHallucinationPhrase(
    const std::vector<TranscriptWord>& words) {
    std::string compact;
    for (const TranscriptWord& word : words) compact += FoldWordText(word.text);
    // ITW9 -- "Sous-titrage" alone: large-v3 signs its credits with other
    // names than the Société Radio-Canada one ("Sous-titrage ST' 501",
    // "Sous-titrage MFP.", met on 14 and 15 November 2026), and the first
    // of them was let through on a conference take.
    static const std::vector<std::string> kKnownPhrases = {
        "soustitrage",    "soustitrespar",
        "abonnezvous",    "noubliezpasdevousabonner",
        "subscribe",      "mercidavoirregarde",
        "generiquedefin",
    };
    return std::any_of(kKnownPhrases.begin(), kKnownPhrases.end(),
                       [&](const std::string& phrase) {
                           return compact.find(phrase) != std::string::npos;
                       });
}

// ITW9 -- the measured speech between `start` and `end`, exact.
RationalTime SpeechBetween(const RationalTime& start, const RationalTime& end,
                           const SpeechOnsetReport& envelope) {
    RationalTime covered{0, 100};
    for (const SpeechGroup& group : envelope.groups) {
        const RationalTime& from = start < group.start ? group.start : start;
        const RationalTime& to = group.end < end ? group.end : end;
        if (from < to) covered = covered.add(to.sub(from));
    }
    return covered;
}

// ITW9 -- what whisper says when it may not say "..." and hears nothing it
// can decode: a phrase repeated over the span ("Merci. Merci. Merci.",
// measured on 15 November 2026), four words or more of which fewer than
// half are distinct.
bool WordsLoop(const std::vector<TranscriptWord>& words) {
    if (words.size() < 4) return false;
    std::set<std::string> distinct;
    for (const TranscriptWord& word : words)
        distinct.insert(FoldWordText(word.text));
    return distinct.size() * 2 < words.size();
}

}  // namespace

bool IsEllipsisOnlyWord(const std::string& text) {
    static constexpr std::string_view kEllipsis = "…";
    size_t dots = 0;
    size_t ellipses = 0;
    for (size_t index = 0; index < text.size();) {
        if (text[index] == '.') {
            ++dots;
            ++index;
        } else if (text[index] == ' ') {
            ++index;
        } else if (text.compare(index, kEllipsis.size(), kEllipsis) == 0) {
            ++ellipses;
            index += kEllipsis.size();
        } else {
            return false;
        }
    }
    return dots >= 2 || ellipses > 0;
}

RationalTime EllipsisOverSpeech(const Transcript& transcript,
                                const SpeechOnsetReport& envelope) {
    RationalTime covered{0, 100};
    if (transcript.media_id != envelope.media_id) return covered;
    for (const TranscriptWord& word : transcript.words) {
        if (!IsEllipsisOnlyWord(word.text)) continue;
        covered = covered.add(SpeechBetween(word.start, word.end, envelope));
    }
    return covered;
}

bool FillEllipsesFromPass(Transcript& transcript, const Transcript& redo,
                          const SpeechOnsetReport& envelope,
                          EllipsisRepair& repair) {
    repair = EllipsisRepair{};
    if (transcript.media_id != envelope.media_id ||
        redo.media_id != transcript.media_id) {
        return false;
    }
    const RationalTime minimum{kEllipsisSpanMinSpeechCentis, 100};
    std::vector<TranscriptWord> kept;
    // Each accepted ellipsis, with the words that would take its place.
    std::vector<std::pair<TranscriptWord, std::vector<TranscriptWord>>> fills;
    for (const TranscriptWord& word : transcript.words) {
        if (!IsEllipsisOnlyWord(word.text) ||
            SpeechBetween(word.start, word.end, envelope) < minimum) {
            kept.push_back(word);
            continue;
        }
        ++repair.spans;
        // Twice the middle against twice the bounds: exact, no division.
        const RationalTime low = word.start.add(word.start);
        const RationalTime high = word.end.add(word.end);
        std::vector<TranscriptWord> inside;
        for (const TranscriptWord& candidate : redo.words) {
            const RationalTime middle = candidate.start.add(candidate.end);
            if (!IsEllipsisOnlyWord(candidate.text) && low <= middle &&
                middle <= high) {
                inside.push_back(candidate);
            }
        }
        if (inside.empty()) {
            kept.push_back(word);
            continue;
        }
        // Words per minute of covered speech, against the floor of
        // likely_incomplete, in exact integers.
        const RationalTime covered =
            SpeechBetween(word.start, word.end, envelope);
        const bool sparse =
            static_cast<__int128>(inside.size()) * 60 * covered.rate <
            static_cast<__int128>(kSparseTranscriptMinWordsPerMinute) *
                covered.value;
        if (ContainsKnownHallucinationPhrase(inside) || WordsLoop(inside) ||
            sparse) {
            ++repair.refused;
            kept.push_back(word);
            continue;
        }
        fills.emplace_back(word, std::move(inside));
    }
    if (fills.empty()) return false;

    // Each word with whether it was put in place (true) or kept (false).
    std::vector<std::pair<TranscriptWord, bool>> merged;
    merged.reserve(kept.size() + redo.words.size());
    for (const TranscriptWord& word : kept) merged.emplace_back(word, false);
    for (const auto& [ellipsis, fill] : fills) {
        size_t placed = 0;
        for (const TranscriptWord& word : fill) {
            const bool overlaps = std::any_of(
                kept.begin(), kept.end(), [&](const TranscriptWord& other) {
                    return word.start < other.end && other.start < word.end;
                });
            if (overlaps) continue;
            merged.emplace_back(word, true);
            ++placed;
        }
        if (placed == 0) merged.emplace_back(ellipsis, false);
    }
    // The kept words are disjoint among themselves, as are the words of one
    // pass, and no word put in place overlaps a kept one. What is left is an
    // ellipsis that stayed for want of words, against a word put in place
    // for its neighbour: the word put in place gives way.
    std::stable_sort(merged.begin(), merged.end(),
                     [](const auto& left, const auto& right) {
                         return left.first.start < right.first.start;
                     });
    std::vector<std::pair<TranscriptWord, bool>> ordered;
    for (auto& entry : merged) {
        if (entry.second) {
            if (!ordered.empty() &&
                entry.first.start < ordered.back().first.end) {
                continue;
            }
        } else {
            while (!ordered.empty() && ordered.back().second &&
                   entry.first.start < ordered.back().first.end) {
                ordered.pop_back();
            }
        }
        ordered.push_back(std::move(entry));
    }
    std::vector<TranscriptWord> words;
    for (auto& [word, placed] : ordered) {
        if (placed) ++repair.words;
        words.push_back(std::move(word));
    }
    // An ellipsis counts as filled once none of it remains.
    for (const auto& fill : fills) {
        const bool remains = std::any_of(
            words.begin(), words.end(), [&](const TranscriptWord& word) {
                return word.text == fill.first.text &&
                       word.start == fill.first.start &&
                       word.end == fill.first.end;
            });
        if (!remains) ++repair.filled;
    }
    if (repair.words == 0) return false;
    transcript.words = std::move(words);
    transcript.ellipsis_pass_words += repair.words;
    // Assessed again by the caller, on what the words now are.
    transcript.speech_assessed = false;
    transcript.measured_speech_duration = {0, 1};
    transcript.likely_hallucinated = false;
    transcript.likely_incomplete = false;
    transcript.known_hallucination_phrase = false;
    return true;
}

bool IsHesitationToken(const std::string& text) {
    const std::string folded = FoldWordText(text);
    if (folded.empty()) return false;
    // ITW9 -- the English and Spanish rushes of PRJK hesitate in "uh",
    // "um" and "eh", which the French lexicon of IsFillerWord does not know.
    // Kept apart from it: FindDisfluencies cuts what that lexicon names, and
    // "ah" or "eh" are words in a French sentence; here a token is only kept,
    // never cut.
    static const std::set<std::string> kOthers = {"uh", "um", "er", "erm",
                                                  "eh", "ah", "m"};
    return IsFillerWord(folded) ||
           kOthers.count(CollapseRepeatedLetters(folded)) != 0;
}

Transcript CompleteVerbatimWithPlainPass(const Transcript& verbatim,
                                         const Transcript& plain,
                                         size_t& keptVerbatimWords) {
    Transcript merged = verbatim;
    // Assessed again by the caller, on what the words now are.
    merged.speech_assessed = false;
    merged.measured_speech_duration = {0, 1};
    merged.likely_hallucinated = false;
    merged.likely_incomplete = false;
    merged.known_hallucination_phrase = false;
    // One detection over the whole media rather than one per window: the
    // plain pass's, which decoded the words.
    if (!plain.detected_language.empty()) {
        merged.detected_language = plain.detected_language;
    }
    merged.words = plain.words;
    merged.plain_pass_words = plain.words.size();
    keptVerbatimWords = 0;
    const RationalTime longest{kMaxKeptHesitationCentis, 100};
    for (const TranscriptWord& word : verbatim.words) {
        // A degenerate verbatim pass stretches one word over seconds; a
        // hesitation is short.
        if (!IsHesitationToken(word.text) || longest < word.end.sub(word.start))
            continue;
        const bool heard = std::any_of(plain.words.begin(), plain.words.end(),
                                       [&](const TranscriptWord& other) {
                                           return word.start < other.end &&
                                                  other.start < word.end;
                                       });
        if (heard) continue;
        merged.words.push_back(word);
        ++keptVerbatimWords;
    }
    // The plain pass's words are disjoint among themselves, and each kept
    // hesitation overlaps none of them nor, from the same verbatim pass,
    // another: ordering them by start keeps every word whole.
    std::stable_sort(
        merged.words.begin(), merged.words.end(),
        [](const TranscriptWord& left, const TranscriptWord& right) {
            return left.start < right.start;
        });
    return merged;
}

bool AssessTranscriptAgainstSpeech(const Transcript& transcript,
                                   const SpeechOnsetReport& envelope,
                                   TranscriptSpeechAssessment& assessment,
                                   std::string& error) {
    assessment = TranscriptSpeechAssessment{};
    if (transcript.media_id != envelope.media_id) {
        error = "speech envelope media_id does not match transcript media_id";
        return false;
    }
    if (envelope.windows_per_second == 0) {
        error = "speech envelope has no valid analysis rate";
        return false;
    }
    RationalTime duration{0, static_cast<int32_t>(envelope.windows_per_second)};
    RationalTime previousEnd{0,
                             static_cast<int32_t>(envelope.windows_per_second)};
    for (const SpeechGroup& group : envelope.groups) {
        const RationalTime start = group.start.rescale(duration.rate);
        const RationalTime end = group.end.rescale(duration.rate);
        if (start < previousEnd || end <= start) {
            error = "speech envelope contains invalid or overlapping groups";
            return false;
        }
        duration = duration.add(end.sub(start));
        previousEnd = end;
    }
    assessment.word_count = transcript.words.size();
    assessment.measured_speech_duration = duration;
    assessment.known_hallucination_phrase =
        ContainsKnownHallucinationPhrase(transcript.words);
    // The phrase list is deliberately not part of the decision. It only
    // raises the evidence level shown to the caller. A voiced interview about
    // subtitles must remain usable; words over zero measured speech cannot.
    assessment.likely_hallucinated =
        assessment.word_count > 0 && duration.value == 0;
    const bool enoughSpeech =
        duration.value >=
        static_cast<int64_t>(duration.rate) * kSparseTranscriptMinSpeechSeconds;
    const __int128 wordsPerMinuteLeft =
        static_cast<__int128>(assessment.word_count) * 60 * duration.rate;
    const __int128 minimumWordsRight =
        static_cast<__int128>(kSparseTranscriptMinWordsPerMinute) *
        duration.value;
    assessment.likely_incomplete =
        enoughSpeech && wordsPerMinuteLeft < minimumWordsRight;
    error.clear();
    return true;
}

void ApplyTranscriptSpeechAssessment(
    Transcript& transcript, const TranscriptSpeechAssessment& assessment) {
    transcript.speech_assessed = true;
    transcript.measured_speech_duration = assessment.measured_speech_duration;
    transcript.likely_hallucinated = assessment.likely_hallucinated;
    transcript.likely_incomplete = assessment.likely_incomplete;
    transcript.known_hallucination_phrase =
        assessment.known_hallucination_phrase;
}

bool TakePlainPassOverIncompleteVerbatim(Transcript& transcript,
                                         const Transcript& plain,
                                         const SpeechOnsetReport& envelope,
                                         TranscriptionOutcome& outcome) {
    outcome.plain_pass_tried = true;
    outcome.plain_pass_used = false;
    outcome.verbatim_pass_words = transcript.words.size();
    outcome.plain_pass_words = plain.words.size();
    outcome.kept_verbatim_words = 0;
    TranscriptSpeechAssessment plainAssessment;
    std::string error;
    if (!AssessTranscriptAgainstSpeech(plain, envelope, plainAssessment,
                                       error)) {
        outcome.plain_pass_detail =
            "the plain pass cannot be assessed: " + error;
        return false;
    }
    if (plainAssessment.likely_hallucinated) {
        outcome.plain_pass_detail =
            "the plain pass is flagged likely_hallucinated";
        return false;
    }
    if (plain.words.size() <= transcript.words.size()) {
        outcome.plain_pass_detail =
            "the plain pass decoded no more words than the verbatim one";
        return false;
    }
    size_t kept = 0;
    Transcript merged = CompleteVerbatimWithPlainPass(transcript, plain, kept);
    TranscriptSpeechAssessment assessment;
    if (!AssessTranscriptAgainstSpeech(merged, envelope, assessment, error)) {
        outcome.plain_pass_detail =
            "the completed transcript cannot be assessed: " + error;
        return false;
    }
    ApplyTranscriptSpeechAssessment(merged, assessment);
    outcome.speech_assessed = true;
    outcome.measured_speech_duration = assessment.measured_speech_duration;
    outcome.likely_hallucinated = assessment.likely_hallucinated;
    outcome.likely_incomplete = assessment.likely_incomplete;
    outcome.known_hallucination_phrase = assessment.known_hallucination_phrase;
    outcome.plain_pass_used = true;
    outcome.kept_verbatim_words = kept;
    outcome.plain_pass_detail.clear();
    transcript = std::move(merged);
    return true;
}

std::vector<Disfluency> FindDisfluencies(
    const std::vector<TranscriptWord>& words) {
    std::vector<std::string> folded;
    folded.reserve(words.size());
    for (const TranscriptWord& word : words)
        folded.push_back(FoldWordText(word.text));

    const auto textOf = [&](size_t from, size_t to) {
        std::string text;
        for (size_t index = from; index <= to; ++index) {
            if (!text.empty()) text.push_back(' ');
            text += words[index].text;
        }
        return text;
    };

    std::vector<Disfluency> found;
    size_t index = 0;
    while (index < words.size()) {
        if (folded[index].empty()) {
            ++index;
            continue;
        }
        if (IsFillerWord(folded[index])) {
            size_t last = index;
            while (last + 1 < words.size() && IsFillerWord(folded[last + 1]))
                ++last;
            found.push_back(
                {{index, last}, DisfluencyKind::Filler, textOf(index, last)});
            index = last + 1;
            continue;
        }
        // A stutter is the same word said twice in a row. The last occurrence
        // is the one kept: it is the one that runs into the word that
        // follows, so closing the cut before it leaves the sentence's own
        // rhythm intact.
        size_t last = index;
        while (last + 1 < words.size() && folded[last + 1] == folded[index])
            ++last;
        if (last > index) {
            found.push_back({{index, last - 1},
                             DisfluencyKind::Repetition,
                             textOf(index, last - 1)});
            index = last;
            continue;
        }
        // AGENT-2026-09 A35 -- a discourse marker, after the stutter check
        // so a doubled "donc" stays a stutter.
        if (const size_t marker = MarkerLength(folded, index)) {
            found.push_back({{index, index + marker - 1},
                             DisfluencyKind::Marker,
                             textOf(index, index + marker - 1)});
            index += marker;
            continue;
        }
        ++index;
    }
    return found;
}

std::vector<Disfluency> FindDisfluenciesInClip(const DocumentClip& clip,
                                               const Transcript& transcript) {
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    std::vector<Disfluency> kept;
    for (Disfluency& item : FindDisfluencies(transcript.words)) {
        const TranscriptWord& first =
            transcript.words[item.range.start_word_index];
        const TranscriptWord& last =
            transcript.words[item.range.end_word_index];
        if (first.start < clip.source_in || clipEnd < last.end) continue;
        kept.push_back(std::move(item));
    }
    return kept;
}

bool ResolveWordRemoval(const DocumentClip& clip, const Transcript& transcript,
                        const std::vector<WordRange>& wordRanges,
                        const RationalTime& gapPadding,
                        const std::vector<Ulid>& syncTrackIds,
                        RemoveWordsOperation& operation, std::string& error) {
    error.clear();
    if (transcript.media_id != clip.source_id) {
        error = "transcript media_id does not match the clip's source_id";
        return false;
    }
    if (wordRanges.empty()) {
        error = "RemoveWords requires at least one word range";
        return false;
    }
    std::vector<WordRemovalRange> ranges;
    ranges.reserve(wordRanges.size());
    bool first = true;
    size_t previousEndIndex = 0;
    for (const WordRange& range : wordRanges) {
        if (range.start_word_index > range.end_word_index ||
            range.end_word_index >= transcript.words.size()) {
            error = "word range indices are out of bounds";
            return false;
        }
        if (!first && range.start_word_index <= previousEndIndex) {
            error = "word ranges must be sorted and non-overlapping";
            return false;
        }
        const TranscriptWord& firstWord =
            transcript.words[range.start_word_index];
        const TranscriptWord& lastWord = transcript.words[range.end_word_index];
        ranges.push_back({firstWord.start, lastWord.end});
        previousEndIndex = range.end_word_index;
        first = false;
    }
    // linked_clip_ids stays empty here: this function resolves *which
    // frames*, and which other clips share the cut is a document-shape
    // question its caller answers (see McpTools.cc's clean_disfluencies,
    // which fills it from the clip's link group).
    operation = RemoveWordsOperation{clip.id, std::move(ranges), gapPadding,
                                     {},      syncTrackIds,      {}};
    return true;
}

bool ResolveWhisperModel(
    const std::string& configured, const std::string& settingsFile,
    const std::function<bool(const std::string&)>& isRegularFile,
    std::string& path, std::string& reason) {
    if (configured.empty()) {
        reason = std::string("no Whisper model configured: set ") +
                 kWhisperModelVariable +
                 " to a local ggml model file, either in the environment or "
                 "in '" +
                 settingsFile + "'";
        return false;
    }
    if (!isRegularFile(configured)) {
        reason = std::string(kWhisperModelVariable) +
                 " does not name a regular file: '" + configured +
                 "' (configured in '" + settingsFile + "')";
        return false;
    }
    path = configured;
    reason.clear();
    return true;
}

std::string ConfiguredWhisperHesitationsModel() {
    return local_env::Value(kWhisperHesitationsModelVariable);
}

bool ResolveConfiguredWhisperModel(std::string& path, std::string& reason) {
    return ResolveWhisperModel(
        local_env::Value(kWhisperModelVariable), local_env::LocalEnvFilePath(),
        [](const std::string& candidate) {
            std::error_code error;
            return std::filesystem::is_regular_file(candidate, error);
        },
        path, reason);
}
