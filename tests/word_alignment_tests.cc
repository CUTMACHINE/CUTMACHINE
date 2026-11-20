#include "InterviewShort.h"
#include "Transcription.h"
#include "Ulid.h"
#include "WordAlignment.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

// ITW3 -- ROADMAP.md. The forced-alignment cache replaces Whisper's word
// boundaries in every word-level reader, and only while it still describes
// the same text.

namespace {

const std::string kMediaId = "01K4AL00000000000000000001";

Transcript Whisper() {
    Transcript transcript;
    transcript.media_id = kMediaId;
    transcript.whisper_model = "ggml-large-v3.bin";
    transcript.language = "fr";
    transcript.verbatim = true;
    transcript.source_rate = {25, 1};
    // Whisper groups "en fait," into one word and places it late.
    transcript.words = {{"Donc", {0, 25}, {6, 25}},
                        {"en fait,", {8, 25}, {20, 25}},
                        {"c'est", {22, 25}, {30, 25}}};
    return transcript;
}

std::string Manifest(const std::string& words, bool textOverride = false,
                     int rate = 25) {
    return std::string("{\"schema_version\":1,\"sample_id\":\"Rushes A\",") +
           "\"media_id\":\"" + kMediaId +
           "\",\"language\":\"fr\",\"source_rate\":{\"num\":" +
           std::to_string(rate) +
           ",\"den\":1},\"alignment\":{\"method\":\"nemo\",\"version\":"
           "\"NeMo 3.0.0\",\"model\":\"stt_fr_conformer_ctc_large\","
           "\"timing_provenance\":\"nfa-word-ctm\",\"reference_text_sha256\":"
           "null},\"processing\":{\"wall_time_ms\":10,"
           "\"audio_duration_frames\":100},\"reference\":{\"text_override\":" +
           (textOverride ? "true" : "false") + "},\"words\":[" + words + "]}";
}

std::string Word(const std::string& text, int start, int end) {
    return "{\"text\":\"" + text +
           "\",\"frame_start\":" + std::to_string(start) +
           ",\"frame_end\":" + std::to_string(end) +
           ",\"confidence\":null,\"speaker_id\":null,\"filler_word\":false,"
           "\"silence_after_frames\":0,\"silence_after_ms\":0}";
}

void Write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-word-alignment");
    const std::filesystem::path cache = root / ".cutmachine";
    const std::filesystem::path alignments =
        cache / "word-alignments" / kMediaId;
    std::string error;
    const std::string aligned = Word("Donc", 0, 5) + "," + Word("en", 7, 9) +
                                "," + Word("fait,", 9, 15) + "," +
                                Word("c'est", 16, 24);

    {
        WordAlignment alignment;
        Write(root / "valid.json", Manifest(aligned));
        Check(LoadWordAlignment((root / "valid.json").string(), alignment,
                                error) &&
                  alignment.words.size() == 4 && alignment.method == "nemo" &&
                  alignment.words[1].frame_start == 7,
              "a bench manifest loads: " + error);
        std::string reason;
        Check(WordAlignmentMatchesTranscript(alignment, Whisper(), reason),
              "NeMo's finer split of the same text matches: " + reason);
        Transcript changed = Whisper();
        changed.words[2].text = "c'était";
        Check(!WordAlignmentMatchesTranscript(alignment, changed, reason) &&
                  reason.find("changed") != std::string::npos,
              "a transcript whose text changed is not matched");
        Transcript otherRate = Whisper();
        otherRate.source_rate = {50, 1};
        Check(!WordAlignmentMatchesTranscript(alignment, otherRate, reason),
              "another source frame rate is not matched");
        Write(root / "override.json", Manifest(aligned, true));
        Check(LoadWordAlignment((root / "override.json").string(), alignment,
                                error) &&
                  WordAlignmentMatchesTranscript(alignment, changed, reason),
              "an alignment of a deliberately replaced text still applies");
    }
    {
        WordAlignment alignment;
        Write(root / "overlap.json",
              Manifest(Word("Donc", 0, 5) + "," + Word("en", 4, 9)));
        Check(!LoadWordAlignment((root / "overlap.json").string(), alignment,
                                 error),
              "overlapping words are refused");
        Write(root / "outside.json", Manifest(Word("Donc", 90, 101)));
        Check(!LoadWordAlignment((root / "outside.json").string(), alignment,
                                 error),
              "a word past the audio duration is refused");
    }

    Transcript whisper = Whisper();
    Check(SaveAudioTranscript(
              (cache / "transcripts" / (kMediaId + ".json")).string(), whisper,
              error),
          "the Whisper cache is written: " + error);
    {
        Transcript words;
        Check(LoadSourceWords(cache, kMediaId, words, error) &&
                  words.word_timing == "whisper" && words.words.size() == 3,
              "without an alignment the Whisper words are read: " + error);
    }
    Write(alignments / "stale.json",
          Manifest(Word("autre", 0, 5) + "," + Word("texte", 6, 9)));
    {
        Transcript words;
        Check(LoadSourceWords(cache, kMediaId, words, error) &&
                  words.word_timing == "whisper",
              "a stale alignment is ignored, not an error: " + error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(alignments / "nemo.json", Manifest(aligned));
    {
        Transcript words;
        Check(LoadSourceWords(cache, kMediaId, words, error) &&
                  words.word_timing == "nemo" && words.words.size() == 4 &&
                  words.words[0].end == RationalTime{5, 25} &&
                  words.words[2].text == "fait," &&
                  words.words[2].start == RationalTime{9, 25} &&
                  words.verbatim && words.whisper_model == "ggml-large-v3.bin",
              "a matching alignment replaces the boundaries and keeps the "
              "metadata: " +
                  error);
        Check(!SaveAudioTranscript(
                  (cache / "transcripts" / (kMediaId + ".json")).string(),
                  words, error) &&
                  error.find("refusing") != std::string::npos,
              "aligned words are never written into the Whisper cache");
        Transcript reread;
        Check(LoadAudioTranscript(
                  (cache / "transcripts" / (kMediaId + ".json")).string(),
                  reread, error) &&
                  reread.words.size() == 3,
              "the Whisper cache is left untouched");
    }

    {
        // The timeline transcript is built from the aligned words.
        Document document;
        document.sources = {{kMediaId, "rush.mov", {25, 1}, {100, 25}}};
        DocumentClip clip;
        clip.source_id = kMediaId;
        clip.source_in = {0, 25};
        clip.duration = {30, 25};
        clip.timeline_in = {0, 25};
        document.sequence.tracks = {{GenerateUlid(), "audio", 0, {clip}}};
        std::vector<TimelineTranscriptSpan> spans;
        Check(BuildTimelineTranscriptWordSpans(document, cache / "transcripts",
                                               spans, error) &&
                  spans.size() == 4 && spans[1].text == "en" &&
                  spans[1].timeline_in == RationalTime{7, 25},
              "word spans follow the aligned boundaries: " + error);
    }

    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All word alignment tests passed\n";
    return 0;
}
