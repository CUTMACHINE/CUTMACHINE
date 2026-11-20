#include "WordAlignment.h"

#include "Json.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

using mcp_json::Value;

bool ReadPositiveInt(const Value* value, int64_t minimum, int64_t& output) {
    return value != nullptr && value->IsNumber() && value->AsInt64(output) &&
           output >= minimum;
}

const std::string* ReadText(const Value& object, const std::string& key) {
    const Value* value = object.Find(key);
    return value != nullptr && value->IsString() && !value->AsString().empty()
               ? &value->AsString()
               : nullptr;
}

// Letters and digits of a word list, ASCII lowercased, every non-ASCII byte
// kept as is. Both lists come from the same Whisper text, so case and
// accents agree; only spacing, punctuation and how words are grouped differ
// (NeMo splits "en fait," into two words).
std::string Letters(const std::vector<std::string>& texts) {
    std::string letters;
    for (const std::string& text : texts) {
        for (const char character : text) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte >= 0x80)
                letters.push_back(character);
            else if (std::isalnum(byte) != 0)
                letters.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return letters;
}

}  // namespace

bool LoadWordAlignment(const std::string& path, WordAlignment& alignment,
                       std::string& error) {
    alignment = {};
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open word alignment";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Value root;
    if (!Value::Parse(buffer.str(), root, error) || !root.IsObject()) {
        error = "word alignment is not a JSON object: " + error;
        return false;
    }
    int64_t schema = 0;
    if (!ReadPositiveInt(root.Find("schema_version"), 1, schema) ||
        schema != 1) {
        error = "word alignment schema_version must be 1";
        return false;
    }
    const std::string* mediaId = ReadText(root, "media_id");
    const Value* rate = root.Find("source_rate");
    const Value* method = root.Find("alignment");
    const Value* processing = root.Find("processing");
    const Value* words = root.Find("words");
    int64_t num = 0;
    int64_t den = 0;
    if (mediaId == nullptr || rate == nullptr || !rate->IsObject() ||
        !ReadPositiveInt(rate->Find("num"), 1, num) ||
        !ReadPositiveInt(rate->Find("den"), 1, den) ||
        num > std::numeric_limits<int32_t>::max() ||
        den > std::numeric_limits<int32_t>::max() || method == nullptr ||
        !method->IsObject() || ReadText(*method, "method") == nullptr ||
        ReadText(*method, "model") == nullptr || processing == nullptr ||
        !processing->IsObject() ||
        !ReadPositiveInt(processing->Find("audio_duration_frames"), 1,
                         alignment.audio_duration_frames) ||
        words == nullptr || !words->IsArray()) {
        error =
            "word alignment lacks media_id, source_rate, alignment, "
            "processing or words";
        return false;
    }
    alignment.path = path;
    alignment.media_id = *mediaId;
    alignment.source_rate = {static_cast<int32_t>(num),
                             static_cast<int32_t>(den)};
    alignment.method = *ReadText(*method, "method");
    alignment.model = *ReadText(*method, "model");
    if (const Value* reference = root.Find("reference");
        reference != nullptr && reference->IsObject()) {
        const Value* override = reference->Find("text_override");
        alignment.text_override =
            override != nullptr && override->IsBool() && override->AsBool();
    }
    int64_t previousEnd = 0;
    for (size_t index = 0; index < words->AsArray().size(); ++index) {
        const Value& item = words->AsArray()[index];
        ForcedAlignedWord word;
        const std::string* text =
            item.IsObject() ? ReadText(item, "text") : nullptr;
        if (text == nullptr ||
            !ReadPositiveInt(item.Find("frame_start"), 0, word.frame_start) ||
            !ReadPositiveInt(item.Find("frame_end"), 1, word.frame_end) ||
            word.frame_end <= word.frame_start ||
            word.frame_start < previousEnd ||
            word.frame_end > alignment.audio_duration_frames) {
            error = "word alignment word " + std::to_string(index) +
                    " is empty, overlapping or outside the audio";
            return false;
        }
        word.text = *text;
        previousEnd = word.frame_end;
        alignment.words.push_back(std::move(word));
    }
    return true;
}

bool WordAlignmentMatchesTranscript(const WordAlignment& alignment,
                                    const Transcript& transcript,
                                    std::string& reason) {
    reason.clear();
    if (alignment.method == "cutmachine") {
        reason = "the alignment carries Whisper's own boundaries";
        return false;
    }
    if (alignment.media_id != transcript.media_id) {
        reason = "the alignment belongs to another media";
        return false;
    }
    if (alignment.source_rate.num != transcript.source_rate.num ||
        alignment.source_rate.den != transcript.source_rate.den) {
        reason = "the alignment uses another source frame rate";
        return false;
    }
    if (alignment.words.empty()) {
        reason = "the alignment has no words";
        return false;
    }
    if (alignment.text_override) return true;
    std::vector<std::string> aligned;
    std::vector<std::string> transcribed;
    for (const ForcedAlignedWord& word : alignment.words)
        aligned.push_back(word.text);
    for (const TranscriptWord& word : transcript.words)
        transcribed.push_back(word.text);
    if (Letters(aligned) != Letters(transcribed)) {
        reason = "the transcript text changed after the alignment";
        return false;
    }
    return true;
}

bool IsForcedAlignedTiming(const std::string& wordTiming) {
    return wordTiming == "mfa" || wordTiming == "nemo" || wordTiming == "gold";
}

void ApplyWordAlignment(const WordAlignment& alignment,
                        Transcript& transcript) {
    transcript.words.clear();
    transcript.words.reserve(alignment.words.size());
    const MediaRate rate = alignment.source_rate;
    for (const ForcedAlignedWord& word : alignment.words) {
        transcript.words.push_back(
            {word.text,
             {word.frame_start * static_cast<int64_t>(rate.den), rate.num},
             {word.frame_end * static_cast<int64_t>(rate.den), rate.num}});
    }
    transcript.word_timing = alignment.method;
}

bool LoadSourceWords(const std::filesystem::path& cacheDirectory,
                     const std::string& mediaId, Transcript& transcript,
                     std::string& error) {
    if (!LoadAudioTranscript(
            (cacheDirectory / "transcripts" / (mediaId + ".json")).string(),
            transcript, error)) {
        return false;
    }
    ApplyNewestWordAlignment(cacheDirectory / "word-alignments" / mediaId,
                             transcript);
    return true;
}

bool ApplyNewestWordAlignment(const std::filesystem::path& directory,
                              Transcript& transcript) {
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(directory, filesystemError))
        return false;
    // Newest first: an alignment rerun after a correction supersedes the
    // earlier one when both still match.
    std::vector<std::pair<std::filesystem::file_time_type, std::string>>
        candidates;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, filesystemError)) {
        if (entry.path().extension() != ".json") continue;
        std::error_code timeError;
        const auto time = std::filesystem::last_write_time(entry, timeError);
        if (!timeError) candidates.emplace_back(time, entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.first != right.first ? left.first > right.first
                                                   : left.second > right.second;
              });
    for (const auto& candidate : candidates) {
        WordAlignment alignment;
        std::string ignored;
        if (LoadWordAlignment(candidate.second, alignment, ignored) &&
            WordAlignmentMatchesTranscript(alignment, transcript, ignored)) {
            ApplyWordAlignment(alignment, transcript);
            return true;
        }
    }
    return false;
}
