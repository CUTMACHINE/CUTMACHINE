// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// speech: transcripts and words, disfluencies, pauses, onsets and hesitations,
// alignment and transcription. Moved out of McpTools.cc as they were; what they
// share with the other files of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

#include "WordAlignment.h"

using namespace mcp_tools;

namespace {

// Adds `cut_words` to a backend result that is a JSON object.
void SetCutWords(std::string& resultJson, Value view) {
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject())
        return;
    result.Set("cut_words", std::move(view));
    resultJson = result.Dump();
}

// IA3 -- docs/roadmap/IA-2026-09.md. The rendered transcript view with the
// revision its ids belong to. Computed here from the view's own spans, the
// way every tool that takes an id recomputes it, so no backend can hand out
// a revision that the tools would then disagree with.
bool ReadTranscriptView(McpBackend& backend, Value& view,
                        std::vector<TimelineTranscriptSpan>& spans,
                        std::string& errorName, std::string& message) {
    std::string json;
    std::string parseError;
    if (!backend.ReadTimelineTranscript(json, message) ||
        !ParseTimelineTranscriptSpans(json, spans, message)) {
        errorName = "IoError";
        return false;
    }
    if (!Value::Parse(json, view, parseError) || !view.IsObject()) {
        errorName = "IoError";
        message = "malformed transcript view: " + parseError;
        return false;
    }
    view.Set("transcript_revision",
             Value::MakeString(TranscriptViewRevision(spans)));
    return true;
}

bool DispatchGetTimelineTranscript(McpBackend& backend, const IdResolver&,
                                   const Value& args, std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    if (!CheckKnownKeys(args, {}, "get_timeline_transcript", message))
        return Fail(errorName, message, message);
    Value view;
    std::vector<TimelineTranscriptSpan> spans;
    if (!ReadTranscriptView(backend, view, spans, errorName, message)) {
        return false;
    }
    resultJson = view.Dump();
    return true;
}

// IA3 -- the same view grouped into blocks of speech (SpeechSelection.h),
// with the view's word timing and word check: a model that reads blocks
// instead of cues keeps every reservation the cues carried.
bool DispatchListSpeechBlocks(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "list_speech_blocks", message))
        return Fail(errorName, message, message);
    Value view;
    std::vector<TimelineTranscriptSpan> spans;
    if (!ReadTranscriptView(backend, view, spans, errorName, message)) {
        return false;
    }
    const Value* timelineId = view.Find("timeline_id");
    Value blocks;
    std::string parseError;
    Value::Parse(SerializeSpeechBlocks(
                     timelineId != nullptr && timelineId->IsString()
                         ? timelineId->AsString()
                         : std::string(),
                     TranscriptViewRevision(spans), BuildSpeechBlocks(spans)),
                 blocks, parseError);
    for (const char* key : {"word_timing", "word_check"}) {
        if (const Value* value = view.Find(key)) {
            blocks.Set(key, *value);
        }
    }
    resultJson = blocks.Dump();
    return true;
}

// IA3 -- reads `transcript_revision` and `segments`, each named by a run of
// spans or a run of blocks. Checked before the transcript is read, so a
// malformed call is told what is wrong with it rather than that the view is
// missing.
bool ReadSpeechSegments(const Value& args, const std::string& path,
                        std::string& revision,
                        std::vector<SpeechSegmentRequest>& requests,
                        std::string& message) {
    const Value* given = args.Find("transcript_revision");
    if (given == nullptr) {
        message = "'" + path +
                  ".transcript_revision' is required: pass the value "
                  "get_timeline_transcript or list_speech_blocks returned "
                  "with the ids, so that an id read from another view is "
                  "refused instead of naming another passage";
        return false;
    }
    if (!given->IsString() || given->AsString().empty()) {
        message = "'" + path + ".transcript_revision' must be a string";
        return false;
    }
    revision = given->AsString();
    const Value* segments = args.Find("segments");
    if (!segments || !segments->IsArray() || segments->AsArray().empty()) {
        message = "'" + path + ".segments' must be a non-empty array";
        return false;
    }
    requests.clear();
    for (size_t index = 0; index < segments->AsArray().size(); ++index) {
        const Value& value = segments->AsArray()[index];
        const std::string at =
            path + ".segments[" + std::to_string(index) + "]";
        SpeechSegmentRequest request;
        if (!CheckKnownKeys(
                value, {"span_id", "end_span_id", "block_id", "end_block_id"},
                at, message) ||
            !ReadString(value, "span_id", at, false, "", request.span_id,
                        message) ||
            !ReadString(value, "end_span_id", at, false, "",
                        request.end_span_id, message) ||
            !ReadString(value, "block_id", at, false, "", request.block_id,
                        message) ||
            !ReadString(value, "end_block_id", at, false, "",
                        request.end_block_id, message)) {
            return false;
        }
        requests.push_back(std::move(request));
    }
    return true;
}

// IA3 -- resolves a selection against the current view, naming a stale one
// StaleSelection so a caller can tell "read again" from "fix the call".
bool ResolveSelectionAgainstView(
    McpBackend& backend, const std::string& path, const std::string& revision,
    const std::vector<SpeechSegmentRequest>& requests,
    std::vector<TimelineTranscriptSpan>& spans,
    std::vector<SpeechSelectionSegment>& segments, std::string& errorName,
    std::string& message) {
    if (!backend.ReadTimelineTranscriptSpans(spans, message)) {
        errorName = "IoError";
        message = "no transcript spans to select from: " + message;
        return false;
    }
    SpeechSelectionError kind = SpeechSelectionError::None;
    if (!ResolveSpeechSelection(spans, revision, requests, path + ".segments",
                                segments, kind, message)) {
        errorName = kind == SpeechSelectionError::Stale ? kStaleSelectionError
                                                        : "ValidationFailed";
        return false;
    }
    return true;
}

bool DispatchInspectSpeechSelection(McpBackend& backend, const IdResolver&,
                                    const Value& args, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    const std::string path = "inspect_speech_selection";
    std::string revision;
    std::vector<SpeechSegmentRequest> requests;
    if (!CheckKnownKeys(args, {"transcript_revision", "segments"}, path,
                        message) ||
        !ReadSpeechSegments(args, path, revision, requests, message))
        return Fail(errorName, message, message);
    std::vector<TimelineTranscriptSpan> spans;
    std::vector<SpeechSelectionSegment> segments;
    if (!ResolveSelectionAgainstView(backend, path, revision, requests, spans,
                                     segments, errorName, message)) {
        return false;
    }
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    resultJson =
        SerializeSpeechSelection(document.sequence.id, revision, segments);
    return true;
}

// ALPHA-2026-08 -- both word tools resolve the clip, then its source's
// cached transcript, then hand word indices to ResolveWordRemoval. The model
// never sees or supplies a timecode: it names words, CUTMACHINE names frames
// (PHILOSOPHY.md principle 7).
bool ResolveClipAndTranscript(McpBackend& backend, const Ulid& clipId,
                              Document& document, const DocumentClip*& clip,
                              Transcript& transcript, std::string& errorName,
                              std::string& message) {
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    clip = document.FindClip(clipId);
    if (clip == nullptr) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (!backend.ReadSourceTranscript(clip->source_id, transcript, message)) {
        errorName = "IoError";
        message = "no cached transcript for source '" + clip->source_id +
                  "': " + message;
        return false;
    }
    return true;
}

bool RefuseLikelyHallucinatedTranscript(const Transcript& transcript,
                                        bool force, const std::string& toolName,
                                        std::string& errorName,
                                        std::string& message) {
    if (!transcript.likely_hallucinated || force) return true;
    errorName = "ValidationFailed";
    message = toolName + " refuses source '" + transcript.media_id +
              "': its transcript has words but no measured speech group; "
              "inspect the rush and pass force:true only to override";
    return false;
}

// AGENT-2026-09 A4 -- ROADMAP.md. The words of one rush with, for each,
// where to enter and leave (WordBoundaries.h). On every measured run the
// agent read the transcript cache and the envelope cache by hand, in
// Python, a dozen times a montage, to answer "where does this word really
// start and stop". Now it asks.
std::string FoldAscii(const std::string& text) {
    std::string folded = text;
    for (char& character : folded)
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    return folded;
}

bool DispatchListSourceWords(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "list_source_words";
    static const std::vector<std::string> kAllowed = {
        "media_id",      "from_frame", "to_frame", "find",
        "context_words", "max_words",  "levels"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    int64_t fromFrame = -1;
    int64_t toFrame = -1;
    std::string find;
    int64_t contextWords = 3;
    int64_t maxWords = 200;
    bool withLevels = false;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadInt64(args, "from_frame", tool, false, fromFrame, fromFrame,
                   message) ||
        !ReadInt64(args, "to_frame", tool, false, toFrame, toFrame, message) ||
        !ReadString(args, "find", tool, false, "", find, message) ||
        !ReadInt64(args, "context_words", tool, false, contextWords,
                   contextWords, message) ||
        !ReadInt64(args, "max_words", tool, false, maxWords, maxWords,
                   message) ||
        !ReadBool(args, "levels", tool, false, withLevels, message))
        return Fail(errorName, message, message);
    if ((args.Find("from_frame") && fromFrame < 0) ||
        (args.Find("to_frame") && toFrame < 0) ||
        (fromFrame >= 0 && toFrame >= 0 && toFrame <= fromFrame))
        return Fail(errorName, message,
                    "'" + tool +
                        "' frames must not be negative, and to_frame must "
                        "be after from_frame");
    if (contextWords < 0 || contextWords > 20)
        return Fail(errorName, message,
                    "'" + tool + ".context_words' must be between 0 and 20");
    if (maxWords < 1 || maxWords > 1000)
        return Fail(errorName, message,
                    "'" + tool + ".max_words' must be between 1 and 1000");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    Transcript transcript;
    if (!backend.ReadSourceTranscript(mediaId, transcript, message)) {
        errorName = "IoError";
        message =
            "no cached transcript for source '" + mediaId + "': " + message;
        return false;
    }
    SpeechOnsetReport envelope;
    std::string envelopeError;
    const bool hasEnvelope =
        backend.ReadSourceSpeechOnset(mediaId, envelope, envelopeError);
    // PA3 -- docs/roadmap/PARITE-2026-11.md. The voice of each word, when
    // analyze_speakers has read the rush.
    SpeakerDiarization speakers;
    std::string speakersError;
    const bool hasSpeakers =
        backend.ReadSourceSpeakers(mediaId, speakers, speakersError);
    const SpeechOnsetThresholds thresholds;
    const WordBoundaryRules rules;
    std::vector<WordBoundary> boundaries;
    std::string error;
    if (!ResolveWordBoundaries(transcript, hasEnvelope ? &envelope : nullptr,
                               thresholds, rules, boundaries, error))
        return Fail(errorName, message, error);

    // Which words to show: the frame range, then the matches of `find` with
    // their neighbours.
    std::vector<bool> keep(boundaries.size(), true);
    if (fromFrame >= 0 || toFrame >= 0) {
        for (size_t index = 0; index < boundaries.size(); ++index) {
            const WordBoundary& word = boundaries[index];
            keep[index] = (fromFrame < 0 || word.end_frame > fromFrame) &&
                          (toFrame < 0 || word.start_frame < toFrame);
        }
    }
    int64_t matches = 0;
    if (!find.empty()) {
        const std::string needle = FoldAscii(find);
        std::vector<bool> shown(boundaries.size(), false);
        for (size_t index = 0; index < boundaries.size(); ++index) {
            if (!keep[index] ||
                FoldAscii(boundaries[index].text).find(needle) ==
                    std::string::npos)
                continue;
            ++matches;
            const size_t first = index >= static_cast<size_t>(contextWords)
                                     ? index - static_cast<size_t>(contextWords)
                                     : 0;
            const size_t end =
                std::min(boundaries.size(),
                         index + static_cast<size_t>(contextWords) + 1);
            for (size_t around = first; around < end; ++around)
                shown[around] = keep[around];
        }
        keep = shown;
    }

    Value words = Value::MakeArray();
    int64_t emitted = 0;
    bool truncated = false;
    int64_t nextFromFrame = -1;
    for (size_t index = 0; index < boundaries.size(); ++index) {
        if (!keep[index]) continue;
        const WordBoundary& word = boundaries[index];
        if (emitted == maxWords) {
            truncated = true;
            nextFromFrame = word.start_frame;
            break;
        }
        Value entry = Value::MakeObject();
        entry.Set("index", Value::MakeInt(static_cast<int64_t>(word.index)));
        entry.Set("text", Value::MakeString(word.text));
        entry.Set("start_frame", Value::MakeInt(word.start_frame));
        entry.Set("end_frame", Value::MakeInt(word.end_frame));
        if (word.has_next)
            entry.Set("gap_after_frames",
                      Value::MakeInt(word.gap_after_frames));
        entry.Set("entry_frame", Value::MakeInt(word.entry_frame));
        entry.Set("exit_frame", Value::MakeInt(word.exit_frame));
        entry.Set("entry_rule", Value::MakeString(word.entry_rule));
        entry.Set("exit_rule", Value::MakeString(word.exit_rule));
        if (hasSpeakers && word.index < transcript.words.size()) {
            const TranscriptWord& said = transcript.words[word.index];
            const int32_t speaker =
                SpeakerOfSpan(speakers, said.start, said.end);
            if (speaker >= 0) entry.Set("speaker", Value::MakeInt(speaker));
        }
        if (!word.voiced) entry.Set("voiced", Value::MakeBool(false));
        // AGENT-2026-09 A14 -- the envelope where the rules decided, in
        // dBFS per frame from three frames before the entry to three after
        // the exit, so a caller sees the voice instead of reading the
        // cache by hand (nine times on a real montage).
        if (withLevels && hasEnvelope) {
            constexpr int64_t kMargin = 3;
            constexpr int64_t kMostFrames = 120;
            const int64_t first =
                std::max<int64_t>(0, word.entry_frame - kMargin);
            const int64_t end =
                std::min(word.exit_frame + kMargin, first + kMostFrames);
            Value levels = Value::MakeArray();
            for (int64_t frame = first; frame < end; ++frame)
                levels.Push(Value::MakeInt(SpeechLevelDbfs(
                    FrameLevel(envelope, transcript.source_rate, frame))));
            entry.Set("levels_from_frame", Value::MakeInt(first));
            entry.Set("levels_dbfs", std::move(levels));
        }
        words.Push(std::move(entry));
        ++emitted;
    }

    Value root = Value::MakeObject();
    root.Set("media_id", Value::MakeString(mediaId));
    if (const LibraryMedia* media = document.FindLibraryMedia(mediaId))
        root.Set("filename", Value::MakeString(media->filename));
    else if (const DocumentSource* source = document.FindSource(mediaId))
        root.Set("filename",
                 Value::MakeString(
                     std::filesystem::path(source->path).filename().string()));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(transcript.source_rate.num));
    rate.Set("den", Value::MakeInt(transcript.source_rate.den));
    root.Set("rate", std::move(rate));
    root.Set("word_timing", Value::MakeString(transcript.word_timing));
    root.Set("verbatim", Value::MakeBool(transcript.verbatim));
    root.Set("decoding_profile", Value::MakeString(WhisperDecodingProfileName(
                                     transcript.decoding_profile)));
    // ITW9 -- the words a plain pass gave a likely_incomplete verbatim one:
    // they carry no hesitation the verbatim pass did not keep.
    if (transcript.plain_pass_words > 0) {
        root.Set(
            "plain_pass_words",
            Value::MakeInt(static_cast<int64_t>(transcript.plain_pass_words)));
    }
    // ITW9 -- words put where whisper had answered "..." over measured
    // speech, from a pass made with the ellipsis forbidden.
    if (transcript.ellipsis_pass_words > 0) {
        root.Set("ellipsis_pass_words", Value::MakeInt(static_cast<int64_t>(
                                            transcript.ellipsis_pass_words)));
    }
    root.Set("likely_hallucinated",
             Value::MakeBool(transcript.likely_hallucinated));
    root.Set("likely_incomplete",
             Value::MakeBool(transcript.likely_incomplete));
    root.Set("envelope", Value::MakeString(hasEnvelope ? "cached" : "missing"));
    if (hasEnvelope) {
        root.Set("threshold_dbfs",
                 Value::MakeInt(SpeechLevelDbfs(
                     WordBoundaryThreshold(envelope, thresholds))));
    } else {
        root.Set("envelope_detail", Value::MakeString(envelopeError));
    }
    Value ruleValues = Value::MakeObject();
    ruleValues.Set("entry_search_frames",
                   Value::MakeInt(rules.entry_search_frames));
    ruleValues.Set("attack_search_frames",
                   Value::MakeInt(rules.attack_search_frames));
    ruleValues.Set("entry_air_frames", Value::MakeInt(rules.entry_air_frames));
    ruleValues.Set("exit_air_frames", Value::MakeInt(rules.exit_air_frames));
    ruleValues.Set("drop_search_frames",
                   Value::MakeInt(rules.drop_search_frames));
    root.Set("rules", std::move(ruleValues));
    root.Set("total_words",
             Value::MakeInt(static_cast<int64_t>(boundaries.size())));
    if (!find.empty()) root.Set("matches", Value::MakeInt(matches));
    // AGENT-2026-09 A18 -- voice where no word is, over the frames asked
    // for (the whole rush by default): a sound nobody wrote, at least 160 ms
    // long, named after the word it follows.
    if (hasEnvelope) {
        const MediaRate rate = transcript.source_rate;
        const int64_t envelopeFrames =
            static_cast<int64_t>(envelope.levels.size()) * rate.num /
            (static_cast<int64_t>(envelope.windows_per_second) * rate.den);
        const int64_t scanFirst = fromFrame >= 0 ? fromFrame : 0;
        const int64_t scanEnd =
            toFrame >= 0 ? std::min(toFrame, envelopeFrames) : envelopeFrames;
        const int64_t minimumFrames =
            (16 * static_cast<int64_t>(rate.num) + 100 * rate.den - 1) /
            (100 * rate.den);
        Value stray = Value::MakeArray();
        for (const VoiceWithoutWord& run : VoiceOutsideWords(
                 boundaries, envelope, thresholds, rate, scanFirst, scanEnd,
                 std::max<int64_t>(1, minimumFrames))) {
            Value entry = Value::MakeObject();
            entry.Set("first_frame", Value::MakeInt(run.first_frame));
            entry.Set("end_frame", Value::MakeInt(run.end_frame));
            entry.Set("frames",
                      Value::MakeInt(run.end_frame - run.first_frame));
            if (run.has_previous) {
                entry.Set("after_word_index",
                          Value::MakeInt(
                              static_cast<int64_t>(run.previous_word_index)));
                entry.Set("after_text",
                          Value::MakeString(
                              boundaries[run.previous_word_index].text));
            }
            stray.Push(std::move(entry));
        }
        root.Set("voice_without_words", std::move(stray));
    }
    // AGENT-2026-09 A23 -- the envelope of the whole range asked for, one
    // dBFS per frame with the speech level and the floor it reads against:
    // the bar chart an agent scripted from the cache, seven times on one
    // montage, to see a pause or a hesitation between two words.
    if (withLevels && hasEnvelope && fromFrame >= 0 && toFrame >= 0) {
        constexpr int64_t kMostRangeFrames = 1000;
        const int64_t end = std::min(toFrame, fromFrame + kMostRangeFrames);
        Value levels = Value::MakeArray();
        for (int64_t frame = fromFrame; frame < end; ++frame)
            levels.Push(Value::MakeInt(SpeechLevelDbfs(
                FrameLevel(envelope, transcript.source_rate, frame))));
        Value range = Value::MakeObject();
        range.Set("from_frame", Value::MakeInt(fromFrame));
        range.Set("end_frame", Value::MakeInt(end));
        range.Set("speech_level_dbfs",
                  Value::MakeInt(SpeechLevelDbfs(envelope.speech_level)));
        range.Set("floor_dbfs",
                  Value::MakeInt(SpeechLevelDbfs(envelope.noise_floor)));
        range.Set("levels_dbfs", std::move(levels));
        // AGENT-2026-09 A37 -- the envelope's own windows too (20 ms at 50
        // a second): a frame of 40 ms was too coarse to place a cut between
        // "là" and "on" on the third run, and the cache was read by hand.
        if (envelope.windows_per_second > 0) {
            const MediaRate rate = transcript.source_rate;
            const int32_t grid =
                static_cast<int32_t>(envelope.windows_per_second);
            const int64_t firstWindow =
                RationalTime{fromFrame * rate.den, rate.num}.to_frames(grid);
            const int64_t endWindow = std::min<int64_t>(
                RationalTime{end * rate.den, rate.num}.to_frames(grid),
                static_cast<int64_t>(envelope.levels.size()));
            Value windows = Value::MakeArray();
            for (int64_t window = std::max<int64_t>(0, firstWindow);
                 window < endWindow; ++window)
                windows.Push(Value::MakeInt(SpeechLevelDbfs(
                    envelope.levels[static_cast<size_t>(window)])));
            range.Set("windows_per_second", Value::MakeInt(grid));
            range.Set("from_window", Value::MakeInt(firstWindow));
            range.Set("window_levels_dbfs", std::move(windows));
        }
        if (end < toFrame) range.Set("truncated", Value::MakeBool(true));
        root.Set("range_levels", std::move(range));
    }
    root.Set("words", std::move(words));
    root.Set("truncated", Value::MakeBool(truncated));
    if (truncated) root.Set("next_from_frame", Value::MakeInt(nextFromFrame));
    resultJson = root.Dump();
    return true;
}

// AGENT-2026-09 A24 -- ROADMAP.md. The montage's words, read from the
// rushes' alignments and projected onto every audio clip (TimelineWords.h):
// what an agent scripted by hand on the second run of the 25 September
// bench, and what transcribe_timeline cost 5 to 30 s of model load to
// approximate.
bool DispatchListTimelineWords(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    const std::string tool = "list_timeline_words";
    static const std::vector<std::string> kAllowed = {"clip_ids", "text_only"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    bool textOnly = false;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message) ||
        !ReadBool(args, "text_only", tool, false, textOnly, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    // Every source an audio clip reads, loaded once.
    std::map<Ulid, TimelineWordSource> sources;
    std::map<Ulid, std::string> unreadable;
    const SpeechOnsetThresholds thresholds;
    const WordBoundaryRules rules;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (sources.count(clip.source_id) ||
                unreadable.count(clip.source_id))
                continue;
            TimelineWordSource source;
            std::string detail;
            if (!backend.ReadSourceTranscript(clip.source_id, source.transcript,
                                              detail)) {
                unreadable[clip.source_id] = detail;
                continue;
            }
            std::string envelopeError;
            source.has_envelope = backend.ReadSourceSpeechOnset(
                clip.source_id, source.envelope, envelopeError);
            std::string error;
            if (!ResolveWordBoundaries(
                    source.transcript,
                    source.has_envelope ? &source.envelope : nullptr,
                    thresholds, rules, source.boundaries, error))
                return Fail(errorName, message,
                            "source '" + clip.source_id + "': " + error);
            sources[clip.source_id] = std::move(source);
        }
    }
    std::vector<TimelineClipWords> clips;
    std::string error;
    if (!ResolveTimelineWords(document, sources, clips, error))
        return Fail(errorName, message, error);
    // AGENT-2026-09 A44 -- what the ear hears and the checks did not say:
    // the hesitation candidates (analyze_hesitations) and the restarts
    // (A43) still inside each clip. On the native run of the 26th, ten
    // "euh" and one "l'Horloge... l'Horloge" were left in the export and
    // no reply had counted them.
    std::map<Ulid, HesitationReport> hesitationReports;
    for (const auto& [sourceId, unused] : sources) {
        HesitationReport report;
        std::string detail;
        if (backend.ReadSourceHesitations(sourceId, report, detail))
            hesitationReports[sourceId] = std::move(report);
    }
    if (!clipIds.empty()) {
        std::vector<TimelineClipWords> kept;
        for (TimelineClipWords& clip : clips)
            if (std::find(clipIds.begin(), clipIds.end(), clip.clip_id) !=
                clipIds.end())
                kept.push_back(std::move(clip));
        clips = std::move(kept);
    }
    const auto filenameOf = [&](const Ulid& sourceId) {
        if (const LibraryMedia* media = document.FindLibraryMedia(sourceId))
            return media->filename;
        if (const DocumentSource* source = document.FindSource(sourceId))
            return std::filesystem::path(source->path).filename().string();
        return std::string();
    };
    Value root = Value::MakeObject();
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(document.sequence.frame_rate.num));
    rate.Set("den", Value::MakeInt(document.sequence.frame_rate.den));
    root.Set("rate", std::move(rate));
    root.Set("text", Value::MakeString(TimelineWordsText(clips)));
    int64_t checked = 0;
    int64_t wordCount = 0;
    int64_t cutInto = 0;
    int64_t unheard = 0;
    int64_t strayClips = 0;
    int64_t silentClips = 0;
    int64_t hesitationsLeft = 0;
    int64_t hesitationRemnants = 0;
    int64_t restartsLeft = 0;
    int64_t hesitationsUnchecked = 0;
    int64_t cutsInVoice = 0;
    Value list = Value::MakeArray();
    for (const TimelineClipWords& clip : clips) {
        if (clip.checked) ++checked;
        wordCount += static_cast<int64_t>(clip.words.size());
        cutInto += (clip.head_cut ? 1 : 0) + (clip.tail_cut ? 1 : 0);
        cutsInVoice +=
            (clip.head_in_voice ? 1 : 0) + (clip.tail_in_voice ? 1 : 0);
        for (const TimelineWord& word : clip.words)
            if (word.whole && !word.voiced) ++unheard;
        if (!clip.stray.empty()) ++strayClips;
        if (clip.silent) ++silentClips;
        // The candidates and restarts the clip still plays, in its source's
        // frames, clamped to the clip.
        Value hesitations = Value::MakeArray();
        Value restarts = Value::MakeArray();
        bool hesitationsKnown = false;
        if (clip.checked) {
            const TimelineWordSource& source = sources.at(clip.source_id);
            const MediaRate rate = source.transcript.source_rate;
            const auto report = hesitationReports.find(clip.source_id);
            if (report != hesitationReports.end()) {
                hesitationsKnown = true;
                for (const HesitationCandidate& candidate :
                     report->second.candidates) {
                    const int64_t first =
                        candidate.start.to_frames(rate.num, rate.den);
                    const int64_t end =
                        candidate.end.to_frames(rate.num, rate.den) + 1;
                    // A48 -- the hold's decay counts with it: what the
                    // clip plays runs to the end of the voice that follows
                    // the candidate (ResolveHeldCut).
                    const HeldCut held = ResolveHeldCut(
                        source.transcript,
                        source.has_envelope ? &source.envelope : nullptr,
                        SpeechOnsetThresholds{}, rate, first, end,
                        candidate.within_word);
                    if (held.voice_end_frame <= clip.source_in_frame ||
                        first >= clip.source_end_frame)
                        continue;
                    const int64_t playedFirst =
                        std::max(first, clip.source_in_frame);
                    const int64_t playedEnd =
                        std::min(held.voice_end_frame, clip.source_end_frame);
                    Value item = Value::MakeObject();
                    item.Set("family", Value::MakeString(candidate.family));
                    item.Set("first_frame", Value::MakeInt(playedFirst));
                    item.Set("end_frame", Value::MakeInt(playedEnd));
                    if (held.voice_end_frame > end)
                        item.Set("trailing_voice_frames",
                                 Value::MakeInt(held.voice_end_frame - end));
                    item.Set("within_word",
                             Value::MakeString(candidate.within_word));
                    item.Set("previous_word",
                             Value::MakeString(candidate.previous_word));
                    // A remnant under 200 ms (the four frames a cut keeps
                    // of a word or of a stretched syllable's head, a frame
                    // or two at a join) is listed but not counted: on the
                    // second native run of the 26th, ten of twelve were
                    // that, and the two that remained were the ones to
                    // hear.
                    constexpr int64_t kRemnantFrames = 3;
                    if (playedEnd - playedFirst < kRemnantFrames) {
                        item.Set("remnant", Value::MakeBool(true));
                        ++hesitationRemnants;
                    } else {
                        ++hesitationsLeft;
                    }
                    hesitations.Push(std::move(item));
                }
            } else {
                ++hesitationsUnchecked;
            }
            if (source.has_envelope &&
                IsForcedAlignedTiming(source.transcript.word_timing)) {
                for (const WordRestart& found : FindWordRestarts(
                         source.transcript, source.envelope,
                         SpeechOnsetThresholds{}, rate, clip.source_in_frame,
                         clip.source_end_frame)) {
                    // A restart the clip no longer plays whole (its first
                    // utterance already cut) is not one.
                    if (found.first_frame < clip.source_in_frame ||
                        found.restart_frame > clip.source_end_frame)
                        continue;
                    Value item = Value::MakeObject();
                    item.Set(
                        "word",
                        Value::MakeString(
                            source.transcript.words[found.word_index].text));
                    item.Set("first_frame", Value::MakeInt(found.first_frame));
                    item.Set("restart_frame",
                             Value::MakeInt(found.restart_frame));
                    item.Set("end_frame", Value::MakeInt(found.end_frame));
                    restarts.Push(std::move(item));
                    ++restartsLeft;
                }
            }
        }
        if (textOnly) continue;
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(clip.clip_id));
        entry.Set("track_id", Value::MakeString(clip.track_id));
        entry.Set("track_index", Value::MakeInt(clip.track_index));
        entry.Set("timeline_in_frame", Value::MakeInt(clip.timeline_in_frame));
        entry.Set("timeline_end_frame",
                  Value::MakeInt(clip.timeline_end_frame));
        entry.Set("source_id", Value::MakeString(clip.source_id));
        entry.Set("filename", Value::MakeString(filenameOf(clip.source_id)));
        entry.Set("checked", Value::MakeBool(clip.checked));
        if (!clip.checked) {
            entry.Set("unchecked_reason",
                      Value::MakeString(clip.unchecked_reason));
            const auto why = unreadable.find(clip.source_id);
            if (why != unreadable.end())
                entry.Set("detail", Value::MakeString(why->second));
            list.Push(std::move(entry));
            continue;
        }
        entry.Set("word_timing", Value::MakeString(clip.word_timing));
        entry.Set("source_in_frame", Value::MakeInt(clip.source_in_frame));
        entry.Set("source_end_frame", Value::MakeInt(clip.source_end_frame));
        std::string text;
        Value words = Value::MakeArray();
        for (const TimelineWord& word : clip.words) {
            if (!text.empty()) text += ' ';
            text += word.text;
            Value item = Value::MakeObject();
            item.Set("index", Value::MakeInt(static_cast<int64_t>(word.index)));
            item.Set("text", Value::MakeString(word.text));
            item.Set("start_frame", Value::MakeInt(word.start_frame));
            item.Set("end_frame", Value::MakeInt(word.end_frame));
            item.Set("timeline_frame", Value::MakeInt(word.timeline_frame));
            if (!word.whole) item.Set("whole", Value::MakeBool(false));
            if (!word.voiced) item.Set("voiced", Value::MakeBool(false));
            words.Push(std::move(item));
        }
        entry.Set("text", Value::MakeString(text));
        entry.Set("words", std::move(words));
        entry.Set("head_cut", Value::MakeBool(clip.head_cut));
        entry.Set("tail_cut", Value::MakeBool(clip.tail_cut));
        if (clip.has_envelope) {
            // A48 -- only when set: a cut inside continuous voice.
            if (clip.head_in_voice)
                entry.Set("head_in_voice", Value::MakeBool(true));
            if (clip.tail_in_voice)
                entry.Set("tail_in_voice", Value::MakeBool(true));
            Value air = Value::MakeObject();
            air.Set("head_frames", Value::MakeInt(clip.head_air_frames));
            air.Set("tail_frames", Value::MakeInt(clip.tail_air_frames));
            air.Set("silent", Value::MakeBool(clip.silent));
            entry.Set("air", std::move(air));
            Value stray = Value::MakeArray();
            const TimelineWordSource& source = sources.at(clip.source_id);
            for (const VoiceWithoutWord& run : clip.stray) {
                Value item = Value::MakeObject();
                item.Set("first_frame", Value::MakeInt(run.first_frame));
                item.Set("end_frame", Value::MakeInt(run.end_frame));
                item.Set("frames",
                         Value::MakeInt(run.end_frame - run.first_frame));
                if (run.has_previous &&
                    run.previous_word_index < source.boundaries.size())
                    item.Set(
                        "after_text",
                        Value::MakeString(
                            source.boundaries[run.previous_word_index].text));
                stray.Push(std::move(item));
            }
            entry.Set("voice_without_words", std::move(stray));
        } else {
            entry.Set("envelope", Value::MakeString("missing"));
        }
        if (hesitationsKnown)
            entry.Set("hesitations", std::move(hesitations));
        else
            entry.Set("hesitations", Value::MakeString("not_analysed"));
        entry.Set("restarts", std::move(restarts));
        list.Push(std::move(entry));
    }
    if (!textOnly) root.Set("clips", std::move(list));
    Value unchecked = Value::MakeArray();
    for (const auto& [sourceId, detail] : unreadable) {
        Value item = Value::MakeObject();
        item.Set("source_id", Value::MakeString(sourceId));
        item.Set("filename", Value::MakeString(filenameOf(sourceId)));
        item.Set("detail", Value::MakeString(detail));
        unchecked.Push(std::move(item));
    }
    root.Set("unchecked_sources", std::move(unchecked));
    Value summary = Value::MakeObject();
    summary.Set("clips", Value::MakeInt(static_cast<int64_t>(clips.size())));
    summary.Set("checked", Value::MakeInt(checked));
    summary.Set("words", Value::MakeInt(wordCount));
    summary.Set("cut_into_words", Value::MakeInt(cutInto));
    summary.Set("cuts_in_voice", Value::MakeInt(cutsInVoice));
    summary.Set("unheard_words", Value::MakeInt(unheard));
    summary.Set("clips_with_stray_voice", Value::MakeInt(strayClips));
    summary.Set("silent_clips", Value::MakeInt(silentClips));
    summary.Set("hesitations_remaining", Value::MakeInt(hesitationsLeft));
    summary.Set("hesitation_remnants", Value::MakeInt(hesitationRemnants));
    summary.Set("restarts_remaining", Value::MakeInt(restartsLeft));
    summary.Set("clips_without_hesitation_analysis",
                Value::MakeInt(hesitationsUnchecked));
    root.Set("summary", std::move(summary));
    resultJson = root.Dump();
    return true;
}

bool DispatchListDisfluencies(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "force"};
    if (!CheckKnownKeys(args, kAllowed, "list_disfluencies", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    bool force = false;
    if (!ReadId(args, "clip_id", "list_disfluencies", resolver, true, clipId,
                message) ||
        !ReadBool(args, "force", "list_disfluencies", false, force, message))
        return Fail(errorName, message, message);
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;
    if (!RefuseLikelyHallucinatedTranscript(
            transcript, force, "list_disfluencies", errorName, message))
        return false;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    root.Set("source_id", Value::MakeString(clip->source_id));
    // Reported because it bounds what this list can contain at all: Whisper's
    // default decoding drops most fillers before they are written down, so an
    // empty result on a non-verbatim transcript says nothing about the take.
    root.Set("verbatim", Value::MakeBool(transcript.verbatim));
    root.Set("decoding_profile", Value::MakeString(WhisperDecodingProfileName(
                                     transcript.decoding_profile)));
    // ITW9 -- nor on the words a plain pass gave a likely_incomplete
    // verbatim one (CompleteVerbatimWithPlainPass).
    if (transcript.plain_pass_words > 0) {
        root.Set(
            "plain_pass_words",
            Value::MakeInt(static_cast<int64_t>(transcript.plain_pass_words)));
    }
    root.Set("words",
             Value::MakeInt(static_cast<int64_t>(transcript.words.size())));
    root.Set("likely_hallucinated",
             Value::MakeBool(transcript.likely_hallucinated));
    root.Set("likely_incomplete",
             Value::MakeBool(transcript.likely_incomplete));
    Value list = Value::MakeArray();
    for (const Disfluency& item : found) {
        Value entry = Value::MakeObject();
        entry.Set(
            "start_word_index",
            Value::MakeInt(static_cast<int64_t>(item.range.start_word_index)));
        entry.Set(
            "end_word_index",
            Value::MakeInt(static_cast<int64_t>(item.range.end_word_index)));
        entry.Set("kind",
                  Value::MakeString(
                      item.kind == DisfluencyKind::Filler   ? "Filler"
                      : item.kind == DisfluencyKind::Marker ? "Marker"
                                                            : "Repetition"));
        entry.Set("text", Value::MakeString(item.text));
        list.Push(std::move(entry));
    }
    root.Set("disfluencies", std::move(list));

    // ITW2 -- ROADMAP.md. What the signal holds that the words do not: the
    // cached hesitation candidates inside this clip, in the rush's frames,
    // each with the range a cut would take. A stretched syllable keeps its
    // first frames so the word still starts.
    // AGENT-2026-09 A47 -- two frames, not four: on the second native run
    // of the 26th the four frames kept of a stretched syllable ("la maaarée",
    // "rivaaage") and of a word next to a "euh" ("le", "un") were what the
    // ear still heard as hesitations once the cuts no longer clicked.
    // Eighty milliseconds carry a syllable's start; a hundred and sixty
    // carry the hesitation too.
    // AGENT-2026-09 A48 -- the cut runs on through the voice that follows
    // the hold, and a word the aligner placed inside the hold is named
    // (ResolveHeldCut, WordBoundaries.h): the editor's twelve marks on that
    // run's export were the decays those cuts had left.
    constexpr int64_t kStretchedKeepFrames = 2;
    HesitationReport hesitations;
    std::string hesitationError;
    const DocumentSource* source = document.FindSource(clip->source_id);
    SpeechOnsetReport envelope;
    std::string envelopeError;
    const bool hasEnvelope =
        source != nullptr &&
        backend.ReadSourceSpeechOnset(clip->source_id, envelope, envelopeError);
    if (source == nullptr ||
        !backend.ReadSourceHesitations(clip->source_id, hesitations,
                                       hesitationError)) {
        root.Set("hesitation_analysis", Value::MakeString("missing"));
    } else {
        root.Set("hesitation_analysis", Value::MakeString("cached"));
        const MediaRate rate = source->rate;
        const RationalTime clipEnd = clip->source_in.add(clip->duration);
        const int64_t clipFirst = clip->source_in.to_frames(rate.num, rate.den);
        const int64_t clipLast = clipEnd.to_frames(rate.num, rate.den);
        Value acoustic = Value::MakeArray();
        for (const HesitationCandidate& candidate : hesitations.candidates) {
            // Frames the candidate touches, on the whole source: the cut is
            // resolved there, then clamped to the clip, so a clip entered
            // on a hold's decay still lists the hold (A48: "le thème" cut
            // in at the dip of the "euh" before it, four frames of decay
            // played, no candidate inside the clip).
            const int64_t first = candidate.start.to_frames(rate.num, rate.den);
            const RationalTime frameStart{
                candidate.end.to_frames(rate.num, rate.den) * rate.den,
                rate.num};
            const int64_t end = candidate.end.to_frames(rate.num, rate.den) +
                                (frameStart < candidate.end ? 1 : 0);
            if (end <= first) continue;
            HeldCutRules rules;
            rules.keep_frames = candidate.family == "stretched_syllable"
                                    ? kStretchedKeepFrames
                                    : 0;
            HeldCut held =
                ResolveHeldCut(transcript, hasEnvelope ? &envelope : nullptr,
                               SpeechOnsetThresholds{}, rate, first, end,
                               candidate.within_word, rules);
            if (held.voice_end_frame <= clipFirst || first >= clipLast)
                continue;
            const int64_t shownFirst = std::max(first, clipFirst);
            const int64_t shownEnd =
                std::max(std::min(end, clipLast), shownFirst);
            held.voice_end_frame = std::min(held.voice_end_frame, clipLast);
            held.cut_first_frame = std::max(held.cut_first_frame, clipFirst);
            held.cut_end_frame = std::min(held.cut_end_frame, clipLast);
            held.cut_including_words_inside_first_frame = std::max(
                held.cut_including_words_inside_first_frame, clipFirst);
            held.cut_including_words_inside_end_frame =
                std::min(held.cut_including_words_inside_end_frame, clipLast);
            std::vector<size_t> wordsInside;
            for (const size_t index : held.words_inside)
                if (transcript.words[index].start.to_frames(
                        rate.num, rate.den) >= clipFirst)
                    wordsInside.push_back(index);
            held.words_inside = std::move(wordsInside);
            Value entry = Value::MakeObject();
            entry.Set("family", Value::MakeString(candidate.family));
            entry.Set("first_frame", Value::MakeInt(shownFirst));
            entry.Set("end_frame", Value::MakeInt(shownEnd));
            if (shownEnd == shownFirst)
                entry.Set("decay_only", Value::MakeBool(true));
            if (held.voice_end_frame > shownEnd)
                entry.Set("voice_end_frame",
                          Value::MakeInt(held.voice_end_frame));
            entry.Set(
                "stability_percent_of_median",
                Value::MakeInt(hesitations.median_stability_milli > 0
                                   ? candidate.stability_milli * 100 /
                                         hesitations.median_stability_milli
                                   : 0));
            entry.Set("level_db_below_speech",
                      Value::MakeInt(candidate.level_db_below_speech));
            entry.Set("previous_word",
                      Value::MakeString(candidate.previous_word));
            entry.Set("within_word", Value::MakeString(candidate.within_word));
            entry.Set("next_word", Value::MakeString(candidate.next_word));
            // AGENT-2026-09 A33 -- the cut never takes a neighbouring
            // aligned word: on the third run a suggested cut took the "le"
            // of "le mix" and the "un" of "un gel", and one cut into "gel".
            // The acoustic range is kept beside it, so the caller sees what
            // was clamped.
            if (held.cut_end_frame > held.cut_first_frame) {
                Value cut = Value::MakeObject();
                cut.Set("first_frame", Value::MakeInt(held.cut_first_frame));
                cut.Set("end_frame", Value::MakeInt(held.cut_end_frame));
                entry.Set("suggested_cut", std::move(cut));
            } else {
                entry.Set("suggested_cut", Value::MakeNull());
                entry.Set("suggested_cut_reason",
                          Value::MakeString("inside_aligned_words"));
            }
            if (held.clamped)
                entry.Set("clamped_to_words", Value::MakeBool(true));
            if (!held.words_inside.empty()) {
                Value inside = Value::MakeArray();
                for (const size_t index : held.words_inside)
                    inside.Push(
                        Value::MakeString(transcript.words[index].text));
                entry.Set("words_inside", std::move(inside));
                if (held.cut_including_words_inside_end_frame >
                    held.cut_including_words_inside_first_frame) {
                    Value cut = Value::MakeObject();
                    cut.Set("first_frame",
                            Value::MakeInt(
                                held.cut_including_words_inside_first_frame));
                    cut.Set("end_frame",
                            Value::MakeInt(
                                held.cut_including_words_inside_end_frame));
                    entry.Set("cut_including_words_inside", std::move(cut));
                }
            }
            acoustic.Push(std::move(entry));
        }
        root.Set("hesitations", std::move(acoustic));
    }
    // AGENT-2026-09 A43 -- a word said twice that the transcript wrote once
    // (FindWordRestarts, WordBoundaries.h): listed with the cut that removes
    // the first utterance whole.
    if (source != nullptr && IsForcedAlignedTiming(transcript.word_timing) &&
        hasEnvelope) {
        const MediaRate rate = source->rate;
        const RationalTime clipEnd = clip->source_in.add(clip->duration);
        Value restarts = Value::MakeArray();
        for (const WordRestart& found : FindWordRestarts(
                 transcript, envelope, SpeechOnsetThresholds{}, rate,
                 clip->source_in.to_frames(rate.num, rate.den),
                 clipEnd.to_frames(rate.num, rate.den))) {
            // A restart whose first utterance the clip no longer plays is
            // done with (A48: listed twice on a montage after its cut).
            if (found.restart_frame <=
                    clip->source_in.to_frames(rate.num, rate.den) ||
                found.first_frame >= clipEnd.to_frames(rate.num, rate.den))
                continue;
            const TranscriptWord& word = transcript.words[found.word_index];
            Value entry = Value::MakeObject();
            entry.Set("family", Value::MakeString("restart_in_word"));
            entry.Set("word", Value::MakeString(word.text));
            entry.Set("word_index",
                      Value::MakeInt(static_cast<int64_t>(found.word_index)));
            entry.Set("first_frame", Value::MakeInt(found.first_frame));
            entry.Set("end_frame", Value::MakeInt(found.end_frame));
            entry.Set("hollow_first_frame",
                      Value::MakeInt(found.hollow_first_frame));
            entry.Set("restart_frame", Value::MakeInt(found.restart_frame));
            entry.Set("previous_word",
                      Value::MakeString(
                          found.word_index > 0
                              ? transcript.words[found.word_index - 1].text
                              : ""));
            entry.Set("next_word",
                      Value::MakeString(
                          found.word_index + 1 < transcript.words.size()
                              ? transcript.words[found.word_index + 1].text
                              : ""));
            // A "euh" ending where the word starts goes with the first
            // utterance: on the second native run the restart was cut from
            // the word's start and five frames of "euh" stayed in front of
            // it.
            int64_t cutFirst = found.first_frame;
            for (const HesitationCandidate& candidate :
                 hesitations.candidates) {
                if (candidate.family != "filler") continue;
                const int64_t candidateFirst =
                    candidate.start.to_frames(rate.num, rate.den);
                const int64_t candidateEnd =
                    candidate.end.to_frames(rate.num, rate.den) + 1;
                if (candidateEnd >= found.first_frame - 2 &&
                    candidateFirst < found.first_frame &&
                    candidateFirst >=
                        clip->source_in.to_frames(rate.num, rate.den))
                    cutFirst = std::min(cutFirst, candidateFirst);
            }
            if (cutFirst < found.first_frame)
                entry.Set("filler_before_frame", Value::MakeInt(cutFirst));
            Value cut = Value::MakeObject();
            cut.Set("first_frame", Value::MakeInt(cutFirst));
            cut.Set("end_frame", Value::MakeInt(found.restart_frame));
            entry.Set("suggested_cut", std::move(cut));
            restarts.Push(std::move(entry));
        }
        root.Set("restarts", std::move(restarts));
    }
    resultJson = root.Dump();
    return true;
}

bool ReadWordRemovalSync(const Value& args, const std::string& tool,
                         const IdResolver& resolver, bool& explicitSync,
                         std::vector<Ulid>& syncTrackIds,
                         std::string& message) {
    explicitSync = args.Find("sync_track_ids") != nullptr;
    return ReadIdArray(args, "sync_track_ids", tool, resolver, syncTrackIds,
                       message);
}

// ITW2 -- ROADMAP.md. Cuts named by source frames rather than by words:
// what a hesitation no transcript wrote can only be addressed by. One
// reversible RemoveWordsOperation, the linked pair cut alike and the
// sync-locked tracks following, like remove_words.
bool DispatchRemoveSourceRanges(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "remove_source_ranges";
    if (!CheckKnownKeys(args, {"clip_id", "ranges", "sync_track_ids"}, tool,
                        message))
        return Fail(errorName, message, message);
    Ulid clipId;
    bool explicitSync = false;
    std::vector<Ulid> syncTrackIds;
    if (!ReadId(args, "clip_id", tool, resolver, true, clipId, message) ||
        !ReadWordRemovalSync(args, tool, resolver, explicitSync, syncTrackIds,
                             message))
        return Fail(errorName, message, message);
    const Value* ranges = args.Find("ranges");
    if (ranges == nullptr || !ranges->IsArray() || ranges->AsArray().empty())
        return Fail(errorName, message,
                    "'remove_source_ranges.ranges' must be a non-empty array");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    const DocumentSource* source =
        clip != nullptr ? document.FindSource(clip->source_id) : nullptr;
    if (clip == nullptr || source == nullptr) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    RemoveWordsOperation operation;
    operation.clip_id = clip->id;
    const MediaRate rate = source->rate;
    for (size_t index = 0; index < ranges->AsArray().size(); ++index) {
        const Value& range = ranges->AsArray()[index];
        const std::string path =
            tool + ".ranges[" + std::to_string(index) + "]";
        int64_t first = 0;
        int64_t end = 0;
        if (!CheckKnownKeys(range, {"first_frame", "end_frame"}, path,
                            message) ||
            !ReadInt64(range, "first_frame", path, true, 0, first, message) ||
            !ReadInt64(range, "end_frame", path, true, 0, end, message))
            return Fail(errorName, message, message);
        if (first < 0 || end <= first)
            return Fail(errorName, message,
                        path + " must hold at least one frame");
        operation.ranges.push_back(
            {{first * rate.den, rate.num}, {end * rate.den, rate.num}});
    }
    std::sort(operation.ranges.begin(), operation.ranges.end(),
              [](const WordRemovalRange& left, const WordRemovalRange& right) {
                  return left.source_start < right.source_start;
              });
    AttachLinkedRemovals(document, *clip, operation);
    operation.sync_track_ids =
        explicitSync ? syncTrackIds
                     : WordRemovalSyncTrackIds(document, operation);
    Value cutWords;
    const bool measured =
        MeasureCutWords(backend, document, operation, {}, cutWords);
    if (!backend.ApplyOperation(operation, resultJson, errorName, message))
        return false;
    if (measured) SetCutWords(resultJson, std::move(cutWords));
    return true;
}

// AGENT-2026-09 A10 -- widens a resolved word removal to the voice the
// rush's envelope hears (WordBoundaries.h) when that envelope is cached,
// and says which frames the cut was placed on: "envelope", or "aligner"
// when there is no envelope, with the reason. A widening that fails is
// reported the same way; the aligner's ranges then stand.
std::string WidenWordRemovalToEnvelope(McpBackend& backend,
                                       const DocumentClip& clip,
                                       const Transcript& transcript,
                                       const std::vector<WordRange>& wordRanges,
                                       RemoveWordsOperation& operation,
                                       int64_t& widenedFrames) {
    widenedFrames = 0;
    SpeechOnsetReport envelope;
    std::string reason;
    if (!backend.ReadSourceSpeechOnset(clip.source_id, envelope, reason))
        return "aligner (no envelope: " + reason + ")";
    RemoveWordsOperation widened = operation;
    std::string error;
    if (!WidenWordRemovalToVoice(
            transcript, envelope, SpeechOnsetThresholds{}, WordBoundaryRules{},
            wordRanges, clip.source_in, clip.source_in.add(clip.duration),
            widened, widenedFrames, error)) {
        widenedFrames = 0;
        return "aligner (" + error + ")";
    }
    operation = std::move(widened);
    return "envelope";
}

bool DispatchRemoveWords(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "ranges", "force", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "remove_words", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    bool force = false;
    bool explicitSync = false;
    std::vector<Ulid> syncTrackIds;
    if (!ReadId(args, "clip_id", "remove_words", resolver, true, clipId,
                message) ||
        !ReadBool(args, "force", "remove_words", false, force, message) ||
        !ReadWordRemovalSync(args, "remove_words", resolver, explicitSync,
                             syncTrackIds, message))
        return Fail(errorName, message, message);
    const Value* ranges = args.Find("ranges");
    if (ranges == nullptr || !ranges->IsArray() || ranges->AsArray().empty())
        return Fail(errorName, message,
                    "'remove_words.ranges' must be a non-empty array");
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;
    if (!RefuseLikelyHallucinatedTranscript(transcript, force, "remove_words",
                                            errorName, message))
        return false;

    std::vector<WordRange> wordRanges;
    for (size_t index = 0; index < ranges->AsArray().size(); ++index) {
        const Value& value = ranges->AsArray()[index];
        const std::string path =
            "remove_words.ranges[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(value, {"start_word_index", "end_word_index"}, path,
                            message))
            return Fail(errorName, message, message);
        int64_t start = 0;
        int64_t end = 0;
        if (!ReadInt64(value, "start_word_index", path, true, 0, start,
                       message) ||
            !ReadInt64(value, "end_word_index", path, true, 0, end, message))
            return Fail(errorName, message, message);
        if (start < 0 || end < 0)
            return Fail(errorName, message,
                        path + " word indices must not be negative");
        wordRanges.push_back(
            {static_cast<size_t>(start), static_cast<size_t>(end)});
    }

    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, wordRanges, RationalTime{0, 1},
                            {}, operation, message))
        return Fail(errorName, message, message);
    // AGENT-2026-09 A10 -- the aligner's end arrives before the voice
    // drops: with the rush's envelope in cache, each range runs to the drop
    // (WordBoundaries.h), so no frame of the word is left behind.
    int64_t widenedFrames = 0;
    const std::string boundaries = WidenWordRemovalToEnvelope(
        backend, *clip, transcript, wordRanges, operation, widenedFrames);
    // The clip's A/V partner loses the same frames, in the same operation.
    // Cutting only the clip named would leave picture and sound at different
    // lengths, which is not something a caller asked for by naming words.
    AttachLinkedRemovals(document, *clip, operation);
    operation.sync_track_ids =
        explicitSync ? syncTrackIds
                     : WordRemovalSyncTrackIds(document, operation);
    // The words named are the targeted ones; any other the cut touches is
    // carried off with them.
    Value cutWords;
    const bool measured =
        MeasureCutWords(backend, document, operation,
                        {{clip->source_id, wordRanges}}, cutWords);
    if (!backend.ApplyOperation(operation, resultJson, errorName, message))
        return false;
    if (measured) SetCutWords(resultJson, std::move(cutWords));
    Value result;
    std::string parseError;
    if (Value::Parse(resultJson, result, parseError) && result.IsObject()) {
        result.Set("boundaries", Value::MakeString(boundaries));
        if (widenedFrames > 0)
            result.Set("widened_frames", Value::MakeInt(widenedFrames));
        resultJson = result.Dump();
    }
    return true;
}

bool DispatchCreateInterviewShort(McpBackend& backend,
                                  const IdResolver& resolver, const Value& args,
                                  std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "name", "segments", "force", "transcript_revision", "activate"};
    const std::string path = "create_interview_short";
    if (!CheckKnownKeys(args, kAllowed, path, message))
        return Fail(errorName, message, message);
    CreateProjectTimelineFromSegmentsOperation operation;
    bool force = false;
    // PRD17 -- ROADMAP.md. The short is activated, as it always was, except
    // in strict timeline mode: there an activation is asked for, never a
    // side effect moving the default under the other agents of a series.
    if (!ReadString(args, "name", path, false, "Short interview — 60 s",
                    operation.name, message) ||
        !ReadBool(args, "force", path, false, force, message) ||
        !ReadBool(args, "activate", path, !backend.RequiresExplicitTimeline(),
                  operation.make_active, message))
        return Fail(errorName, message, message);
    // Segments are named by span or block id and resolved here. This is the
    // whole point of the tool's shape: a caller that could pass a source_in
    // could pass one that lands mid-word, and nothing downstream would catch
    // it -- ApplyOperation only checks that a range sits inside the media.
    // Taking ids instead makes a cut inside a word unrepresentable rather
    // than merely discouraged (PHILOSOPHY.md principle 7).
    //
    // IA3 -- and ids are positions in one view, so the view's revision comes
    // with them: an S12 read before an edit, or from another timeline, is
    // refused as StaleSelection rather than resolved to whatever S12 is now.
    //
    // Arguments are validated before the transcript is read, so a caller
    // still passing the old source_in/duration shape is told which field is
    // wrong instead of being told the transcript is missing -- which it may
    // well also be, and which would send them fixing the wrong thing.
    std::string revision;
    std::vector<SpeechSegmentRequest> requests;
    if (!ReadSpeechSegments(args, path, revision, requests, message))
        return Fail(errorName, message, message);
    std::vector<TimelineTranscriptSpan> spans;
    std::vector<SpeechSelectionSegment> selection;
    if (!ResolveSelectionAgainstView(backend, path, revision, requests, spans,
                                     selection, errorName, message)) {
        return false;
    }
    // The timeline the selection was read in: its links say which recorder's
    // sound goes with each range (SYNCHRO-2026-10 SY3).
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    for (const SpeechSelectionSegment& selected : selection) {
        const bool unsafeSource =
            std::any_of(spans.begin(), spans.end(),
                        [&](const TimelineTranscriptSpan& span) {
                            return span.source_id == selected.source_id &&
                                   span.likely_hallucinated;
                        });
        if (unsafeSource && !force) {
            return Fail(errorName, message,
                        "create_interview_short refuses source '" +
                            selected.source_id +
                            "': its transcript has words but no measured "
                            "speech group; inspect the rush and pass "
                            "force:true only to override");
        }
        ProjectTimelineSourceSegment segment;
        segment.source_id = selected.source_id;
        segment.source_in = selected.source_in;
        segment.duration = selected.duration;
        EditError editError = EditError::None;
        if (!ResolveShortSegmentSound(document, segment, editError, message)) {
            errorName = EditErrorName(editError);
            return false;
        }
        operation.segments.push_back(std::move(segment));
    }
    operation.width = document.sequence.width;
    operation.height = document.sequence.height;
    operation.frame_rate = document.sequence.frame_rate;
    (void)resolver;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool DispatchCleanDisfluencies(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "include_repetitions", "include_markers", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "clean_disfluencies", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    bool includeRepetitions = false;
    bool includeMarkers = false;
    bool explicitSync = false;
    std::vector<Ulid> syncTrackIds;
    if (!ReadId(args, "clip_id", "clean_disfluencies", resolver, true, clipId,
                message) ||
        !ReadBool(args, "include_repetitions", "clean_disfluencies", false,
                  includeRepetitions, message) ||
        !ReadBool(args, "include_markers", "clean_disfluencies", false,
                  includeMarkers, message) ||
        !ReadWordRemovalSync(args, "clean_disfluencies", resolver, explicitSync,
                             syncTrackIds, message))
        return Fail(errorName, message, message);
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    std::vector<WordRange> ranges;
    Value removed = Value::MakeArray();
    for (const Disfluency& item : found) {
        if (item.kind == DisfluencyKind::Repetition && !includeRepetitions)
            continue;
        // AGENT-2026-09 A35 -- a marker carries the sentence as often as
        // not: listed, never removed unless asked.
        if (item.kind == DisfluencyKind::Marker && !includeMarkers) continue;
        ranges.push_back(item.range);
        Value entry = Value::MakeObject();
        entry.Set("kind",
                  Value::MakeString(
                      item.kind == DisfluencyKind::Filler   ? "Filler"
                      : item.kind == DisfluencyKind::Marker ? "Marker"
                                                            : "Repetition"));
        entry.Set("text", Value::MakeString(item.text));
        removed.Push(std::move(entry));
    }

    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    root.Set("verbatim", Value::MakeBool(transcript.verbatim));
    root.Set("decoding_profile", Value::MakeString(WhisperDecodingProfileName(
                                     transcript.decoding_profile)));
    // ITW9 -- what a plain pass completed carries no hesitation to clean.
    if (transcript.plain_pass_words > 0) {
        root.Set(
            "plain_pass_words",
            Value::MakeInt(static_cast<int64_t>(transcript.plain_pass_words)));
    }
    root.Set("removed_count",
             Value::MakeInt(static_cast<int64_t>(ranges.size())));
    root.Set("removed", std::move(removed));
    if (ranges.empty()) {
        // Nothing to cut is a success, not a failure: "clean this clip" on an
        // already clean clip has been honoured. Reporting `verbatim` alongside
        // matters here more than anywhere else -- a standard transcript drops
        // most fillers before they are ever written down, so an empty result
        // on one says nothing about the take.
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, ranges, RationalTime{0, 1}, {},
                            operation, message))
        return Fail(errorName, message, message);
    // AGENT-2026-09 A10 -- a filler is removed to its drop, like any word.
    int64_t widenedFrames = 0;
    root.Set("boundaries", Value::MakeString(WidenWordRemovalToEnvelope(
                               backend, *clip, transcript, ranges, operation,
                               widenedFrames)));
    if (widenedFrames > 0)
        root.Set("widened_frames", Value::MakeInt(widenedFrames));
    AttachLinkedRemovals(document, *clip, operation);
    operation.sync_track_ids =
        explicitSync ? syncTrackIds
                     : WordRemovalSyncTrackIds(document, operation);
    Value linkedView = Value::MakeArray();
    for (const Ulid& linkedId : operation.linked_clip_ids)
        linkedView.Push(Value::MakeString(linkedId));
    // SYNCHRO-2026-10 SY3 -- the recorder's sound under the picture too.
    for (const LinkedWordRemoval& link : operation.translated_links) {
        linkedView.Push(Value::MakeString(link.clip_id));
    }
    root.Set("also_cut", std::move(linkedView));
    Value cutWords;
    if (MeasureCutWords(backend, document, operation,
                        {{clip->source_id, ranges}}, cutWords))
        root.Set("cut_words", std::move(cutWords));
    // MOTIF3 -- the rule and what it was asked to take.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("include_repetitions", Value::MakeBool(includeRepetitions));
    ruleParams.Set("include_markers", Value::MakeBool(includeMarkers));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::Hesitation, "clean_disfluencies",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string operationResult;
    if (!backend.ApplyOperation(operation, operationResult, errorName, message))
        return false;
    root.Set("applied", Value::MakeBool(true));
    // Carry through whatever the apply path reported (doc_hash, and the
    // delta: AGENT-2026-09 A11), so a caller can still detect drift and
    // patch its model after an intent-level tool call.
    Value applied;
    std::string appliedParseError;
    if (Value::Parse(operationResult, applied, appliedParseError) &&
        applied.IsObject()) {
        const Value* hash = applied.Find("doc_hash");
        if (hash != nullptr) root.Set("doc_hash", *hash);
        const Value* delta = applied.Find("delta");
        if (delta != nullptr) root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

// QC-2026-09 A2 -- the pause-closing gesture, from the envelope alone. It
// sits beside clean_disfluencies rather than inside it because the two ask
// different questions of different artifacts: that one removes words a
// speaker said and did not mean, this one removes the time between them, and
// only the first needs a transcript. Both end as one RemoveWordsOperation,
// so both are one undo step.
// AGENT-2026-09 A26 -- ROADMAP.md. One call for many clips or junctions.
// On the two runs of the 25 September bench, tighten_pauses was called nine
// times and close_junction_air nineteen, once per clip or per junction,
// each its own turn. The per-item tool runs once per entry of the list, in
// order, each its own reversible operation; ids are ULIDs, so a ripple
// from one item leaves the next id valid. A failing item stops the batch
// and the refusal says how many items were applied before it.
using ToolDispatcher = bool (*)(McpBackend&, const IdResolver&, const Value&,
                                std::string&, std::string&, std::string&);

bool RunBatch(McpBackend& backend, const IdResolver& resolver,
              const Value& args, const std::string& tool,
              const std::string& listKey,
              const std::vector<std::string>& itemKeys, ToolDispatcher single,
              std::string& resultJson, std::string& errorName,
              std::string& message) {
    const Value* list = args.Find(listKey);
    if (list == nullptr || !list->IsArray() || list->AsArray().empty())
        return Fail(errorName, message,
                    "'" + tool + "." + listKey + "' must be a non-empty array");
    for (const std::string& key : itemKeys)
        if (args.Find(key) != nullptr)
            return Fail(errorName, message,
                        "'" + tool + "' takes '" + key + "' or '" + listKey +
                            "', not both");
    Value results = Value::MakeArray();
    int64_t applied = 0;
    for (const Value& item : list->AsArray()) {
        Value one = Value::MakeObject();
        for (const auto& [key, value] : args.AsObject())
            if (key != listKey) one.Set(key, value);
        if (itemKeys.size() == 1) {
            if (!item.IsString())
                return Fail(
                    errorName, message,
                    "'" + tool + "." + listKey + "' must hold ids as strings");
            one.Set(itemKeys[0], item);
        } else {
            if (!item.IsArray() || item.AsArray().size() != itemKeys.size())
                return Fail(errorName, message,
                            "'" + tool + "." + listKey + "' must hold [" +
                                itemKeys[0] + ", " + itemKeys[1] + "] pairs");
            for (size_t index = 0; index < itemKeys.size(); ++index) {
                if (!item.AsArray()[index].IsString())
                    return Fail(errorName, message,
                                "'" + tool + "." + listKey +
                                    "' must hold ids as strings");
                one.Set(itemKeys[index], item.AsArray()[index]);
            }
        }
        std::string json;
        std::string name;
        std::string detail;
        if (!single(backend, resolver, one, json, name, detail)) {
            errorName = name;
            message = "item " + std::to_string(applied + 1) + " of " +
                      std::to_string(list->AsArray().size()) + " refused (" +
                      std::to_string(applied) + " applied before it, each " +
                      "its own undo step): " + detail;
            return false;
        }
        Value parsed;
        std::string parseError;
        if (!Value::Parse(json, parsed, parseError) || !parsed.IsObject()) {
            errorName = "IoError";
            message = "malformed answer of " + tool + ": " + parseError;
            return false;
        }
        Value entry = Value::MakeObject();
        for (size_t index = 0; index < itemKeys.size(); ++index)
            entry.Set(itemKeys[index], one.Find(itemKeys[index]) != nullptr
                                           ? *one.Find(itemKeys[index])
                                           : Value::MakeNull());
        for (const auto& [key, value] : parsed.AsObject())
            entry.Set(key, value);
        results.Push(std::move(entry));
        ++applied;
    }
    Value root = Value::MakeObject();
    root.Set("ok", Value::MakeBool(true));
    root.Set("applied", Value::MakeInt(applied));
    root.Set("results", std::move(results));
    resultJson = root.Dump();
    return true;
}

bool DispatchTightenPauses(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    if (args.Find("clip_ids") != nullptr)
        return RunBatch(backend, resolver, args, "tighten_pauses", "clip_ids",
                        {"clip_id"}, &DispatchTightenPauses, resultJson,
                        errorName, message);
    static const std::vector<std::string> kAllowed = {
        "clip_id",        "min_gap_ms",    "keep_frames",
        "sync_track_ids", "close_unheard", "unheard_margin_db"};
    if (!CheckKnownKeys(args, kAllowed, "tighten_pauses", message))
        return Fail(errorName, message, message);
    // PRD15 -- one of two arguments, which a schema cannot require of every
    // client: said as such rather than as a "required" clip_id the schema
    // does not list.
    if (args.Find("clip_id") == nullptr)
        return Fail(errorName, message,
                    "'tighten_pauses' takes clip_id, or clip_ids for several "
                    "clips");
    Ulid clipId;
    PauseTighteningSettings settings;
    std::vector<Ulid> syncTrackIds;
    if (!ReadId(args, "clip_id", "tighten_pauses", resolver, true, clipId,
                message) ||
        !ReadInt64(args, "min_gap_ms", "tighten_pauses", false,
                   settings.minimum_gap_milliseconds,
                   settings.minimum_gap_milliseconds, message) ||
        !ReadInt64(args, "keep_frames", "tighten_pauses", false,
                   settings.keep_frames, settings.keep_frames, message) ||
        !ReadBool(args, "close_unheard", "tighten_pauses", false,
                  settings.close_unheard, message) ||
        !ReadInt64(args, "unheard_margin_db", "tighten_pauses", false,
                   settings.unheard_margin_db, settings.unheard_margin_db,
                   message) ||
        !ReadIdArray(args, "sync_track_ids", "tighten_pauses", resolver,
                     syncTrackIds, message))
        return Fail(errorName, message, message);

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr) {
        errorName = EditErrorName(EditError::UnknownSource);
        message = "clip '" + clipId + "' reads from an unmounted source";
        return false;
    }
    SpeechOnsetReport envelope;
    if (!backend.ReadSourceSpeechOnset(clip->source_id, envelope, message)) {
        errorName = "IoError";
        message = "no speech envelope for source '" + clip->source_id +
                  "': " + message;
        return false;
    }

    // IA8 -- aligned words veto the cut. A missing transcript is not an
    // error: the tool closes pauses as before and its report says so.
    Transcript words;
    std::string wordsError;
    const bool haveWords =
        backend.ReadSourceTranscript(clip->source_id, words, wordsError);
    RemoveWordsOperation operation;
    PauseTighteningReport report;
    if (!ResolvePauseTightening(*clip, envelope, haveWords ? &words : nullptr,
                                settings, source->rate, syncTrackIds, operation,
                                report, message))
        return Fail(errorName, message, message);

    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    Value reportView;
    std::string reportParseError;
    if (Value::Parse(SerializePauseTighteningReport(report), reportView,
                     reportParseError))
        root.Set("report", std::move(reportView));
    if (operation.ranges.empty()) {
        // An already tight clip is a success, not a failure: the intent has
        // been honoured. Applying an empty RemoveWords would only be
        // refused, and refusing here would make a caller guess why.
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    AttachLinkedRemovals(document, *clip, operation);
    if (args.Find("sync_track_ids") == nullptr)
        operation.sync_track_ids = WordRemovalSyncTrackIds(document, operation);
    Value linkedView = Value::MakeArray();
    for (const Ulid& linkedId : operation.linked_clip_ids)
        linkedView.Push(Value::MakeString(linkedId));
    // SYNCHRO-2026-10 SY3 -- the recorder's sound under the picture too.
    for (const LinkedWordRemoval& link : operation.translated_links) {
        linkedView.Push(Value::MakeString(link.clip_id));
    }
    root.Set("also_cut", std::move(linkedView));
    // IA9 -- with IA8 an aligned word is never closed over; this is what
    // shows it, and what names the words when no alignment guarded the cut.
    Value cutWords;
    if (MeasureCutWords(backend, document, operation, {}, cutWords))
        root.Set("cut_words", std::move(cutWords));
    // MOTIF3 -- the thresholds the pauses were closed at, defaults included.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("min_gap_ms",
                   Value::MakeInt(settings.minimum_gap_milliseconds));
    ruleParams.Set("keep_frames", Value::MakeInt(settings.keep_frames));
    ruleParams.Set("close_unheard", Value::MakeBool(settings.close_unheard));
    ruleParams.Set("unheard_margin_db",
                   Value::MakeInt(settings.unheard_margin_db));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::Pause, "tighten_pauses",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string operationResult;
    if (!backend.ApplyOperation(operation, operationResult, errorName, message))
        return false;
    root.Set("applied", Value::MakeBool(true));
    Value applied;
    std::string appliedParseError;
    if (Value::Parse(operationResult, applied, appliedParseError) &&
        applied.IsObject()) {
        const Value* hash = applied.Find("doc_hash");
        if (hash != nullptr) root.Set("doc_hash", *hash);
        // AGENT-2026-09 A11 -- the delta every other edit returns: without
        // it a caller re-reads the timeline after each pass.
        const Value* delta = applied.Find("delta");
        if (delta != nullptr) root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

// B4 -- ROADMAP.md. Both boundary tools resolve B1 speech groups into one
// TrimBoundaryAir operation. No transcript is read here: the measured defect
// was precisely that word timestamps can miss the audible edge by 13--22
// frames.
bool DispatchTrimBoundaryAir(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    if (args.Find("clip_ids") != nullptr)
        return RunBatch(backend, resolver, args, "trim_boundary_air",
                        "clip_ids", {"clip_id"}, &DispatchTrimBoundaryAir,
                        resultJson, errorName, message);
    static const std::vector<std::string> kAllowed = {
        "clip_id", "keep_frames", "min_air_ms", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "trim_boundary_air", message))
        return Fail(errorName, message, message);
    // PRD15 -- one of two arguments, as tighten_pauses.
    if (args.Find("clip_id") == nullptr)
        return Fail(errorName, message,
                    "'trim_boundary_air' takes clip_id, or clip_ids for "
                    "several clips");
    Ulid clipId;
    BoundaryAirSettings settings;
    std::vector<Ulid> explicitSync;
    const bool syncExplicit = args.Find("sync_track_ids") != nullptr;
    if (!ReadId(args, "clip_id", "trim_boundary_air", resolver, true, clipId,
                message) ||
        !ReadInt64(args, "keep_frames", "trim_boundary_air", false,
                   settings.keep_frames, settings.keep_frames, message) ||
        !ReadInt64(args, "min_air_ms", "trim_boundary_air", false,
                   settings.minimum_air_milliseconds,
                   settings.minimum_air_milliseconds, message) ||
        !ReadIdArray(args, "sync_track_ids", "trim_boundary_air", resolver,
                     explicitSync, message)) {
        return Fail(errorName, message, message);
    }

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (!source) {
        errorName = EditErrorName(EditError::UnknownSource);
        message = "clip '" + clipId + "' reads from an unmounted source";
        return false;
    }
    SpeechOnsetReport envelope;
    if (!backend.ReadSourceSpeechOnset(clip->source_id, envelope, message)) {
        errorName = "IoError";
        message = "no speech envelope for source '" + clip->source_id +
                  "': " + message;
        return false;
    }

    TrimBoundaryAirOperation operation;
    BoundaryAirReport report;
    if (!ResolveBoundaryAir(*clip, envelope, settings, source->rate, {},
                            operation, report, message)) {
        return Fail(errorName, message, message);
    }
    for (BoundaryAirTrim& trim : operation.trims) {
        const DocumentClip* target = document.FindClip(trim.clip_id);
        if (target)
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, *target, trim.edge);
    }
    operation.sync_track_ids =
        syncExplicit ? explicitSync
                     : BoundarySyncTrackIds(document, operation.trims);

    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    Value reportView;
    std::string parseError;
    if (Value::Parse(SerializeBoundaryAirReport(report), reportView,
                     parseError)) {
        root.Set("report", std::move(reportView));
    }
    if (operation.trims.empty()) {
        root.Set("ok", Value::MakeBool(true));
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    Value alsoCut = Value::MakeArray();
    std::vector<Ulid> reported;
    for (const BoundaryAirTrim& trim : operation.trims) {
        for (const Ulid& linkedId : trim.linked_clip_ids) {
            if (std::find(reported.begin(), reported.end(), linkedId) ==
                reported.end()) {
                reported.push_back(linkedId);
                alsoCut.Push(Value::MakeString(linkedId));
            }
        }
    }
    root.Set("also_cut", std::move(alsoCut));
    Value syncTracks = Value::MakeArray();
    for (const Ulid& trackId : operation.sync_track_ids)
        syncTracks.Push(Value::MakeString(trackId));
    root.Set("sync_track_ids", std::move(syncTracks));
    Value cutWords;
    if (MeasureCutWords(backend, document, operation, {}, cutWords))
        root.Set("cut_words", std::move(cutWords));
    // MOTIF3 -- the air the rule keeps, defaults included.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("keep_frames", Value::MakeInt(settings.keep_frames));
    ruleParams.Set("min_air_ms",
                   Value::MakeInt(settings.minimum_air_milliseconds));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::BoundaryAir, "trim_boundary_air",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string appliedJson;
    if (!backend.ApplyOperation(operation, appliedJson, errorName, message))
        return false;
    root.Set("ok", Value::MakeBool(true));
    root.Set("applied", Value::MakeBool(true));
    Value applied;
    if (Value::Parse(appliedJson, applied, parseError) && applied.IsObject()) {
        const Value* hash = applied.Find("doc_hash");
        if (hash) root.Set("doc_hash", *hash);
        // PRD40 -- ROADMAP.md. The delta every edit returns.
        if (const Value* delta = applied.Find("delta"))
            root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

bool DispatchCloseJunctionAir(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    if (args.Find("pairs") != nullptr)
        return RunBatch(backend, resolver, args, "close_junction_air", "pairs",
                        {"left_clip_id", "right_clip_id"},
                        &DispatchCloseJunctionAir, resultJson, errorName,
                        message);
    static const std::vector<std::string> kAllowed = {
        "left_clip_id", "right_clip_id", "keep_frames", "min_air_ms",
        "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "close_junction_air", message))
        return Fail(errorName, message, message);
    // PRD15 -- one junction or a list of them, as tighten_pauses.
    if (args.Find("left_clip_id") == nullptr &&
        args.Find("right_clip_id") == nullptr)
        return Fail(errorName, message,
                    "'close_junction_air' takes left_clip_id and "
                    "right_clip_id, or pairs for several junctions");
    Ulid leftId;
    Ulid rightId;
    BoundaryAirSettings settings;
    std::vector<Ulid> explicitSync;
    const bool syncExplicit = args.Find("sync_track_ids") != nullptr;
    if (!ReadId(args, "left_clip_id", "close_junction_air", resolver, true,
                leftId, message) ||
        !ReadId(args, "right_clip_id", "close_junction_air", resolver, true,
                rightId, message) ||
        !ReadInt64(args, "keep_frames", "close_junction_air", false,
                   settings.keep_frames, settings.keep_frames, message) ||
        !ReadInt64(args, "min_air_ms", "close_junction_air", false,
                   settings.minimum_air_milliseconds,
                   settings.minimum_air_milliseconds, message) ||
        !ReadIdArray(args, "sync_track_ids", "close_junction_air", resolver,
                     explicitSync, message)) {
        return Fail(errorName, message, message);
    }

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* left = document.FindClip(leftId);
    const DocumentClip* right = document.FindClip(rightId);
    if (!left || !right) {
        errorName = EditErrorName(EditError::UnknownClip);
        message =
            "unknown clip in junction '" + leftId + "' -> '" + rightId + "'";
        return false;
    }
    const DocumentTrack* leftTrack = document.FindTrackForClip(left->id);
    const DocumentTrack* rightTrack = document.FindTrackForClip(right->id);
    if (!leftTrack || !rightTrack || leftTrack->id != rightTrack->id) {
        return Fail(errorName, message,
                    "junction clips must be on the same track");
    }
    const DocumentSource* leftSource = document.FindSource(left->source_id);
    const DocumentSource* rightSource = document.FindSource(right->source_id);
    if (!leftSource || !rightSource) {
        errorName = EditErrorName(EditError::UnknownSource);
        message = "junction clip reads from an unmounted source";
        return false;
    }
    SpeechOnsetReport leftEnvelope;
    SpeechOnsetReport rightEnvelope;
    if (!backend.ReadSourceSpeechOnset(left->source_id, leftEnvelope,
                                       message) ||
        !backend.ReadSourceSpeechOnset(right->source_id, rightEnvelope,
                                       message)) {
        errorName = "IoError";
        message = "missing speech envelope for junction: " + message;
        return false;
    }

    TrimBoundaryAirOperation operation;
    JunctionAirReport report;
    if (!ResolveJunctionAir(*left, leftEnvelope, leftSource->rate, *right,
                            rightEnvelope, rightSource->rate, settings, {},
                            operation, report, message)) {
        return Fail(errorName, message, message);
    }
    for (BoundaryAirTrim& trim : operation.trims) {
        const DocumentClip* target = document.FindClip(trim.clip_id);
        if (target)
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, *target, trim.edge);
    }
    operation.sync_track_ids =
        syncExplicit ? explicitSync
                     : BoundarySyncTrackIds(document, operation.trims);

    Value root = Value::MakeObject();
    root.Set("left_clip_id", Value::MakeString(left->id));
    root.Set("right_clip_id", Value::MakeString(right->id));
    Value reportView;
    std::string parseError;
    if (Value::Parse(SerializeJunctionAirReport(report), reportView,
                     parseError)) {
        root.Set("report", std::move(reportView));
    }
    if (operation.trims.empty()) {
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    Value alsoCut = Value::MakeArray();
    std::vector<Ulid> reported;
    for (const BoundaryAirTrim& trim : operation.trims) {
        for (const Ulid& linkedId : trim.linked_clip_ids) {
            if (std::find(reported.begin(), reported.end(), linkedId) ==
                reported.end()) {
                reported.push_back(linkedId);
                alsoCut.Push(Value::MakeString(linkedId));
            }
        }
    }
    root.Set("also_cut", std::move(alsoCut));
    Value syncTracks = Value::MakeArray();
    for (const Ulid& trackId : operation.sync_track_ids)
        syncTracks.Push(Value::MakeString(trackId));
    root.Set("sync_track_ids", std::move(syncTracks));
    Value cutWords;
    if (MeasureCutWords(backend, document, operation, {}, cutWords))
        root.Set("cut_words", std::move(cutWords));
    // MOTIF3 -- the air the rule keeps, defaults included.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("keep_frames", Value::MakeInt(settings.keep_frames));
    ruleParams.Set("min_air_ms",
                   Value::MakeInt(settings.minimum_air_milliseconds));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::BoundaryAir, "close_junction_air",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string appliedJson;
    if (!backend.ApplyOperation(operation, appliedJson, errorName, message))
        return false;
    root.Set("applied", Value::MakeBool(true));
    Value applied;
    if (Value::Parse(appliedJson, applied, parseError) && applied.IsObject()) {
        const Value* hash = applied.Find("doc_hash");
        if (hash) root.Set("doc_hash", *hash);
        // PRD40 -- ROADMAP.md. The delta every edit returns.
        if (const Value* delta = applied.Find("delta"))
            root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

bool DispatchListSpeechOnsets(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "group_gap_ms", "group_floor_db", "dominant_percentile", "dominance_db",
        "dominant_sustain_windows"};
    if (!CheckKnownKeys(args, kAllowed, "list_speech_onsets", message))
        return Fail(errorName, message, message);
    SpeechOnsetThresholds thresholds;
    if (!ReadInt64(args, "group_gap_ms", "list_speech_onsets", false,
                   thresholds.group_gap_milliseconds,
                   thresholds.group_gap_milliseconds, message) ||
        !ReadInt64(args, "group_floor_db", "list_speech_onsets", false,
                   thresholds.group_floor_db, thresholds.group_floor_db,
                   message) ||
        !ReadInt64(args, "dominant_percentile", "list_speech_onsets", false,
                   thresholds.dominant_percentile,
                   thresholds.dominant_percentile, message) ||
        !ReadInt64(args, "dominance_db", "list_speech_onsets", false,
                   thresholds.dominance_db, thresholds.dominance_db, message) ||
        !ReadInt64(args, "dominant_sustain_windows", "list_speech_onsets",
                   false, thresholds.dominant_sustain_windows,
                   thresholds.dominant_sustain_windows, message))
        return Fail(errorName, message, message);
    if (thresholds.group_gap_milliseconds < 20 ||
        thresholds.group_gap_milliseconds > 60000 ||
        thresholds.group_floor_db < 0 || thresholds.group_floor_db > 120 ||
        thresholds.dominant_percentile < 1 ||
        thresholds.dominant_percentile > 100 || thresholds.dominance_db < 0 ||
        thresholds.dominance_db > 120 ||
        thresholds.dominant_sustain_windows < 1 ||
        thresholds.dominant_sustain_windows > 100000)
        return Fail(errorName, message,
                    "speech onset thresholds are outside their documented "
                    "bounds");
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    std::map<Ulid, SpeechOnsetReport> reports;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (reports.count(clip.source_id)) continue;
            SpeechOnsetReport report;
            std::string readError;
            if (backend.ReadSourceSpeechOnset(clip.source_id, report,
                                              readError))
                reports.emplace(clip.source_id, std::move(report));
        }
    }
    resultJson = DescribeSpeechOnsetForAgent(document, reports, thresholds);
    return true;
}

bool DispatchAnalyzeHesitations(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "analyze_hesitations";
    if (!CheckKnownKeys(args, {"media_id", "media_ids", "include_windows"},
                        tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    bool batch = false;
    bool includeWindows = false;
    if (!ReadAnalysisMediaIds(args, tool, resolver, mediaIds, batch, message) ||
        !ReadBool(args, "include_windows", tool, false, includeWindows,
                  message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return RunAnalysisBatch(
        mediaIds, batch, tool,
        [&](const Ulid& mediaId, std::string& json, std::string& error) {
            HesitationReport report;
            if (!backend.AnalyzeSourceHesitations(mediaId, HesitationSettings{},
                                                  report, error))
                return false;
            json = SerializeHesitationReport(report, includeWindows);
            // AGENT-2026-09 A36 -- the candidates also in the source's
            // frames, the unit every cutting tool takes; the 10 ms grid
            // stays beside them.
            const DocumentSource* source = document.FindSource(mediaId);
            Value parsed;
            std::string parseError;
            if (source == nullptr || source->rate.num <= 0 ||
                !Value::Parse(json, parsed, parseError) || !parsed.IsObject())
                return true;
            const MediaRate rate = source->rate;
            Value rebuilt = Value::MakeObject();
            for (const auto& [key, value] : parsed.AsObject()) {
                if (key != "candidates" || !value.IsArray()) {
                    rebuilt.Set(key, value);
                    continue;
                }
                Value list = Value::MakeArray();
                for (const Value& item : value.AsArray()) {
                    Value entry = Value::MakeObject();
                    if (item.IsObject())
                        for (const auto& [field, inner] : item.AsObject())
                            entry.Set(field, inner);
                    RationalTime start;
                    RationalTime end;
                    std::string timeError;
                    if (item.IsObject() &&
                        ReadTime(item, "start", tool, true, start, timeError) &&
                        ReadTime(item, "end", tool, true, end, timeError)) {
                        const int64_t first =
                            start.to_frames(rate.num, rate.den);
                        int64_t last = end.to_frames(rate.num, rate.den);
                        const RationalTime lastStart{last * rate.den, rate.num};
                        if (end.sub(lastStart).value > 0) ++last;
                        entry.Set("start_frame", Value::MakeInt(first));
                        entry.Set("end_frame", Value::MakeInt(last));
                    }
                    list.Push(std::move(entry));
                }
                rebuilt.Set("candidates", std::move(list));
            }
            json = rebuilt.Dump();
            return true;
        },
        resultJson, errorName, message);
}

bool DispatchAnalyzeSpeechOnset(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id", "media_ids", "group_gap_ms", "group_floor_db"};
    if (!CheckKnownKeys(args, kAllowed, "analyze_speech_onset", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    bool batch = false;
    SpeechOnsetSettings settings;
    if (!ReadAnalysisMediaIds(args, "analyze_speech_onset", resolver, mediaIds,
                              batch, message) ||
        !ReadInt64(args, "group_gap_ms", "analyze_speech_onset", false,
                   settings.thresholds.group_gap_milliseconds,
                   settings.thresholds.group_gap_milliseconds, message) ||
        !ReadInt64(args, "group_floor_db", "analyze_speech_onset", false,
                   settings.thresholds.group_floor_db,
                   settings.thresholds.group_floor_db, message))
        return Fail(errorName, message, message);
    if (settings.thresholds.group_gap_milliseconds < 20 ||
        settings.thresholds.group_gap_milliseconds > 60000 ||
        settings.thresholds.group_floor_db < 0 ||
        settings.thresholds.group_floor_db > 120)
        return Fail(errorName, message,
                    "speech group settings are outside their documented "
                    "bounds");
    return RunAnalysisBatch(
        mediaIds, batch, "analyze_speech_onset",
        [&](const Ulid& mediaId, std::string& json, std::string& error) {
            return backend.AnalyzeSourceSpeechOnset(mediaId, settings, json,
                                                    error);
        },
        resultJson, errorName, message);
}

// QC-2026-09 (A1) -- the pass the agent used to run by hand, forty probes at
// a time. `apply` defaults to false so reading what the signal contradicts
// stays free of consequence; setting it installs the corrected boundaries in
// the transcript cache every word-level tool in this catalog reads.
bool DispatchAlignTranscript(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"apply"};
    if (!CheckKnownKeys(args, kAllowed, "align_transcript", message))
        return Fail(errorName, message, message);
    bool apply = false;
    if (!ReadBool(args, "apply", "align_transcript", false, apply, message))
        return Fail(errorName, message, message);
    return backend.AlignSourceTranscripts(apply, resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

// AX2 -- docs/roadmap/AX-2026-09.md. MFA's boundaries, run by the engine
// instead of by an agent's shell. `texts` keys are resolved like any media
// reference, so an alias from describe names a rush here too.
bool DispatchAlignWords(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    const std::string tool = "align_words";
    static const std::vector<std::string> kAllowed = {"media_ids", "texts"};
    std::vector<Ulid> mediaIds;
    if (!CheckKnownKeys(args, kAllowed, tool, message) ||
        !ReadIdArray(args, "media_ids", tool, resolver, mediaIds, message))
        return Fail(errorName, message, message);
    std::string textsJson;
    if (const Value* texts = args.Find("texts")) {
        if (!texts->IsObject()) {
            message = "'align_words.texts' must be an object {media_id: text}";
            return Fail(errorName, message, message);
        }
        Value resolved = Value::MakeObject();
        for (const auto& [key, text] : texts->AsObject()) {
            Ulid id;
            if (!resolver.Resolve(tool + ".texts", key, id, message))
                return Fail(errorName, message, message);
            if (!text.IsString()) {
                message = "'align_words.texts." + key + "' must be a string";
                return Fail(errorName, message, message);
            }
            resolved.Set(id, text);
        }
        textsJson = resolved.Dump();
    }
    return backend.AlignSourceWords(mediaIds, textsJson, resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

// PA3 -- docs/roadmap/PARITE-2026-11.md. Who speaks when in one rush. The
// count of voices is the caller's: the model groups, it does not count.
bool DispatchAnalyzeSpeakers(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "analyze_speakers";
    static const std::vector<std::string> kAllowed = {"media_id", "speakers"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    int64_t speakers = 0;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadInt64(args, "speakers", tool, true, 0, speakers, message))
        return Fail(errorName, message, message);
    if (speakers < 1 || speakers > kMaxDiarizedSpeakers)
        return Fail(errorName, message,
                    "'" + tool + ".speakers' must lie in [1, " +
                        std::to_string(kMaxDiarizedSpeakers) +
                        "]: how many voices the rush holds");
    return backend.AnalyzeSourceSpeakers(
               mediaId, static_cast<int32_t>(speakers), resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

// PA3 -- the cache analyze_speakers wrote, as turns: each stretch one voice
// holds, with the words said in it, so that the reader can tell who a
// number is. It measures and does not name.
bool DispatchListSpeakers(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    const std::string tool = "list_speakers";
    static const std::vector<std::string> kAllowed = {"media_id", "max_turns",
                                                      "from_turn"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    int64_t maxTurns = 100;
    int64_t fromTurn = 0;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadInt64(args, "max_turns", tool, false, maxTurns, maxTurns,
                   message) ||
        !ReadInt64(args, "from_turn", tool, false, fromTurn, fromTurn, message))
        return Fail(errorName, message, message);
    if (maxTurns < 1 || maxTurns > 500 || fromTurn < 0)
        return Fail(errorName, message,
                    "'" + tool +
                        "' takes max_turns from 1 to 500 and a from_turn "
                        "that is not negative");
    SpeakerDiarization speakers;
    if (!backend.ReadSourceSpeakers(mediaId, speakers, message)) {
        errorName = "IoError";
        message = "no speakers cache for source '" + mediaId +
                  "' (run analyze_speakers first): " + message;
        return false;
    }
    Transcript transcript;
    std::string transcriptError;
    const bool hasWords =
        backend.ReadSourceTranscript(mediaId, transcript, transcriptError);

    // A turn: consecutive stretches of one voice, whatever the pauses
    // between them, until another voice speaks.
    struct Turn {
        int32_t speaker = 0;
        RationalTime start;
        RationalTime end;
    };
    std::vector<Turn> turns;
    for (const SpeakerSegment& segment : speakers.segments) {
        if (!turns.empty() && turns.back().speaker == segment.speaker) {
            turns.back().end = segment.end;
            continue;
        }
        turns.push_back({segment.speaker, segment.start, segment.end});
    }
    const auto time = [](const RationalTime& value) {
        Value item = Value::MakeObject();
        item.Set("value", Value::MakeInt(value.value));
        item.Set("rate", Value::MakeInt(value.rate));
        return item;
    };
    Value listed = Value::MakeArray();
    size_t word = 0;
    int64_t emitted = 0;
    for (size_t index = 0; index < turns.size(); ++index) {
        const Turn& turn = turns[index];
        // The words of the turn: those whose middle falls in it, the first
        // twelve spelled, all counted.
        std::string text;
        int64_t count = 0;
        int64_t firstWord = -1;
        if (hasWords) {
            while (word < transcript.words.size()) {
                const TranscriptWord& said = transcript.words[word];
                const RationalTime middle = said.start.add(said.end);
                // middle / 2 < turn.end, without dividing.
                if (!(middle < turn.end.add(turn.end))) break;
                if (!(middle < turn.start.add(turn.start))) {
                    if (firstWord < 0) firstWord = static_cast<int64_t>(word);
                    if (count < 12)
                        text += (text.empty() ? "" : " ") + said.text;
                    ++count;
                }
                ++word;
            }
        }
        if (static_cast<int64_t>(index) < fromTurn || emitted >= maxTurns)
            continue;
        Value item = Value::MakeObject();
        item.Set("turn", Value::MakeInt(static_cast<int64_t>(index)));
        item.Set("speaker", Value::MakeInt(turn.speaker));
        item.Set("start", time(turn.start));
        item.Set("end", time(turn.end));
        if (hasWords) {
            item.Set("words", Value::MakeInt(count));
            if (firstWord >= 0)
                item.Set("first_word", Value::MakeInt(firstWord));
            item.Set("text",
                     Value::MakeString(count > 12 ? text + " …" : text));
        }
        listed.Push(std::move(item));
        ++emitted;
    }
    Value voices = Value::MakeArray();
    const std::vector<RationalTime> durations = SpeakerDurations(speakers);
    for (size_t speaker = 0; speaker < durations.size(); ++speaker) {
        Value item = Value::MakeObject();
        item.Set("speaker", Value::MakeInt(static_cast<int64_t>(speaker)));
        item.Set("duration", time(durations[speaker]));
        int64_t held = 0;
        for (const Turn& turn : turns)
            held += turn.speaker == static_cast<int32_t>(speaker) ? 1 : 0;
        item.Set("turns", Value::MakeInt(held));
        voices.Push(std::move(item));
    }
    Value similarity = Value::MakeArray();
    for (const SpeakerSimilarity& pair : speakers.similarity) {
        Value item = Value::MakeObject();
        item.Set("speakers", [&] {
            Value both = Value::MakeArray();
            both.Push(Value::MakeInt(pair.first));
            both.Push(Value::MakeInt(pair.second));
            return both;
        }());
        item.Set("per_mille", Value::MakeInt(pair.per_mille));
        similarity.Push(std::move(item));
    }
    Value result = Value::MakeObject();
    result.Set("media_id", Value::MakeString(mediaId));
    result.Set("speakers", Value::MakeInt(speakers.speakers));
    result.Set("speakers_heard", Value::MakeInt(speakers.speakers_heard));
    result.Set("speech_from", Value::MakeString(speakers.speech_from));
    result.Set("embedding", Value::MakeString(speakers.embedding));
    result.Set("voices", std::move(voices));
    result.Set("similarity", std::move(similarity));
    result.Set("turn_count",
               Value::MakeInt(static_cast<int64_t>(turns.size())));
    result.Set("turns", std::move(listed));
    if (fromTurn + emitted < static_cast<int64_t>(turns.size()))
        result.Set("next_from_turn", Value::MakeInt(fromTurn + emitted));
    resultJson = result.Dump();
    return true;
}

bool DispatchTranscribeMedia(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id",       "media_ids", "language",        "verbatim",
        "include_silent", "overwrite", "decoding_profile"};
    if (!CheckKnownKeys(args, kAllowed, "transcribe_media", message))
        return Fail(errorName, message, message);
    // QC-2026-09 A3 -- one media or a list, never both. The single form stays
    // because most calls are one media and building an array for it would be
    // noise; the list form is what makes the model load once instead of once
    // per rush.
    Ulid mediaId;
    std::vector<Ulid> mediaIds;
    if (!ReadId(args, "media_id", "transcribe_media", resolver, false, mediaId,
                message) ||
        !ReadIdArray(args, "media_ids", "transcribe_media", resolver, mediaIds,
                     message))
        return Fail(errorName, message, message);
    if (!mediaId.empty() && !mediaIds.empty())
        return Fail(errorName, message,
                    "'transcribe_media' takes either 'media_id' or "
                    "'media_ids', not both");
    if (!mediaId.empty()) mediaIds.push_back(mediaId);
    if (mediaIds.empty())
        return Fail(errorName, message,
                    "'transcribe_media' requires 'media_id' or 'media_ids'");
    std::string language;
    if (!ReadString(args, "language", "transcribe_media", false, "auto",
                    language, message))
        return Fail(errorName, message, message);
    bool verbatim = false;
    bool includeSilent = false;
    // PRD40 -- install the new transcript even where it looks worse than
    // the cached one (CachedTranscriptPreferredReason).
    bool overwrite = false;
    if (!ReadBool(args, "verbatim", "transcribe_media", false, verbatim,
                  message) ||
        !ReadBool(args, "include_silent", "transcribe_media", false,
                  includeSilent, message) ||
        !ReadBool(args, "overwrite", "transcribe_media", false, overwrite,
                  message))
        return Fail(errorName, message, message);
    std::string profileName;
    WhisperDecodingProfile profile;
    if (!ReadString(args, "decoding_profile", "transcribe_media", false,
                    "default", profileName, message) ||
        !ParseWhisperDecodingProfile(profileName, profile, message) ||
        !ValidateWhisperDecodingRequest(profile, language, verbatim, message))
        return Fail(errorName, message, message);
    return backend.TranscribeSources(mediaIds, language, verbatim,
                                     includeSilent, overwrite, resultJson,
                                     message, profile)
               ? true
               : Fail(errorName, message, message);
}

bool DispatchTranscribeTimeline(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "timeline_id", "language", "verbatim", "decoding_profile"};
    if (!CheckKnownKeys(args, kAllowed, "transcribe_timeline", message))
        return Fail(errorName, message, message);
    std::string timelineId;
    std::string language;
    bool verbatim = false;
    if (!ReadString(args, "timeline_id", "transcribe_timeline", false, "",
                    timelineId, message) ||
        !ReadString(args, "language", "transcribe_timeline", false, "auto",
                    language, message) ||
        !ReadBool(args, "verbatim", "transcribe_timeline", false, verbatim,
                  message)) {
        return Fail(errorName, message, message);
    }
    std::string profileName;
    WhisperDecodingProfile profile;
    if (!ReadString(args, "decoding_profile", "transcribe_timeline", false,
                    "default", profileName, message) ||
        !ParseWhisperDecodingProfile(profileName, profile, message) ||
        !ValidateWhisperDecodingRequest(profile, language, verbatim, message))
        return Fail(errorName, message, message);
    return backend.TranscribeTimeline(timelineId, language, verbatim,
                                      resultJson, message, profile)
               ? true
               : Fail(errorName, message, message);
}

}  // namespace

void RegisterSpeechTools(const McpToolAdder& add) {
    add("get_timeline_transcript",
        "Read the cached transcript of audible clips as selectable semantic "
        "spans. Name spans by span_id with the transcript_revision this "
        "returns: ids are positions in this reading. The exact times are shown "
        "so a human can check them; never retype one into another tool. A span "
        "with straddles_cut=true contains a word cut at an edit and may be "
        "approximate. likely_hallucinated=true means words exist without any "
        "measured speech group and selection tools refuse them unless "
        "force:true is explicit. likely_incomplete=true means sustained "
        "measured speech produced too few words, so missing text must not be "
        "treated as silence.",
        SchemaBuilder().Build("get_timeline_transcript arguments"),
        DispatchGetTimelineTranscript);

    add("list_speech_blocks",
        "Read the transcript view grouped into blocks of speech: runs of spans "
        "cut where a sentence ends, where the speaker pauses longer than a "
        "breath, or where the rush changes. Choose ideas here rather than "
        "subtitle cues. opens_mid_sentence or closes_mid_sentence mean the "
        "block is not a whole sentence; flags that are false are left out. "
        "word_timing and word_check are those of get_timeline_transcript.",
        SchemaBuilder().Build("list_speech_blocks arguments"),
        DispatchListSpeechBlocks);

    const std::string speechSegmentSchema =
        "{\"type\":\"object\",\"properties\":{\"span_id\":" +
        StringSchema(
            "First transcript span of this segment, as returned by "
            "get_timeline_transcript (for example \"S12\").") +
        ",\"end_span_id\":" +
        StringSchema(
            "Optional last span of a contiguous run, inclusive. Use it when "
            "one idea covers several spans.") +
        ",\"block_id\":" +
        StringSchema(
            "First block of this segment, as returned by "
            "list_speech_blocks (for example \"B4\"), in place of "
            "span_id.") +
        ",\"end_block_id\":" +
        StringSchema("Optional last block of a contiguous run, inclusive.") +
        "},\"additionalProperties\":false}";

    const std::string revisionDescription =
        "The transcript_revision returned by get_timeline_transcript or "
        "list_speech_blocks with the ids used here.";

    add("inspect_speech_selection",
        "Check a selection before building it, without changing anything. For "
        "each segment, in order: its exact range and text, whether it opens or "
        "closes mid-sentence, and what the same rush says right before and "
        "right after it (the rest of that sentence), with kept (the selection "
        "keeps it somewhere) and heard_next_to_it (the neighbouring segment "
        "plays it). overlaps_segments names segments that replay the same "
        "material.",
        SchemaBuilder()
            .Field("transcript_revision", StringSchema(revisionDescription),
                   true)
            .Field("segments",
                   ArraySchema(speechSegmentSchema,
                               "Segments in final editorial order, each "
                               "named by spans or by blocks."),
                   true)
            .Build("inspect_speech_selection arguments"),
        DispatchInspectSpeechSelection);

    add("create_interview_short",
        "Create a new timeline from transcript spans or speech blocks, leaving "
        "the original timeline unchanged. Name segments by span_id "
        "(get_timeline_transcript) or block_id (list_speech_blocks), with the "
        "transcript_revision read alongside them; inspect_speech_selection "
        "shows first what each segment leaves out around it. A rush synced to "
        "a recorder (sync_audio) keeps its sound, on a third track, linked; "
        "the camera's sound track is turned off.",
        SchemaBuilder()
            .Field("name", StringSchema("Name of the new timeline."), false)
            .Field("transcript_revision", StringSchema(revisionDescription),
                   true)
            .Field("segments",
                   ArraySchema(speechSegmentSchema,
                               "Segments in final editorial order, each "
                               "named by spans or by blocks."),
                   true)
            .Field("force",
                   BoolSchema("Override the refusal of transcript spans with "
                              "likely_hallucinated=true. Defaults to false."),
                   false)
            .Field("activate",
                   BoolSchema("Make the short the active timeline; default "
                              "true, false in strict timeline mode."),
                   false)
            .Build("create_interview_short arguments"),
        DispatchCreateInterviewShort);

    add("clean_disfluencies",
        "Remove every detected hesitation from one clip, without the caller "
        "enumerating anything. Needs a verbatim transcript; the result says "
        "which kind it read." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to clean."), true)
            .Field("include_repetitions",
                   BoolSchema("Also remove immediately repeated words. "
                              "Defaults to false."),
                   false)
            .Field("include_markers",
                   BoolSchema("true: also remove the discourse markers "
                              "list_disfluencies lists as Marker (donc, "
                              "enfin, en fait, en gros, du coup...). Default "
                              "false: a marker often carries the sentence."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema(
                       "Tracks that follow the cut beyond the linked pair; "
                       "default: every unlocked sync-locked track (a cutaway "
                       "over removed words shortens with them)."),
                   false)
            .Build("clean_disfluencies arguments"),
        DispatchCleanDisfluencies);

    add("tighten_pauses",
        "Close the silences inside one clip: find every hollow in its speech "
        "envelope at least min_gap_ms long, cut each down to keep_frames, and "
        "ripple the clip closed, carrying its linked A/V pair. Needs no "
        "transcript, only a cached speech onset analysis for the clip's "
        "source. Air at the clip's head or tail is reported and left alone; "
        "that is a head trim, see list_speech_onsets. When a forced word "
        "alignment is cached, no hollow is closed over an aligned word: the "
        "silences either side are judged on their own, and report.kept_words "
        "names the word. Without one, report.words_checked is false." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to tighten."), false)
            .Field("clip_ids",
                   IdArraySchema("Several clips instead of clip_id, in this "
                                 "order, one undo step each; one result per "
                                 "clip under `results`."),
                   false)
            .Field("min_gap_ms",
                   IntSchema("Shortest hollow treated as a pause, in "
                             "milliseconds (default 320). Below it, a hollow "
                             "is a breath and removing it sounds clipped."),
                   false)
            .Field("keep_frames",
                   IntSchema("Frames of each pause to keep, in the source's "
                             "own frame rate (default 4). Split between the "
                             "two ends, so the splice lands in the deepest "
                             "part of the hollow."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema(
                       "Tracks that follow the cut beyond the linked pair; "
                       "default: every unlocked sync-locked track (a cutaway "
                       "over removed words shortens with them)."),
                   false)
            .Field("close_unheard",
                   BoolSchema("true: also close over aligned words nobody said "
                              "(within unheard_margin_db of the floor), named "
                              "under closed_over_words; default false: kept, "
                              "heard:false under kept_words. Check one with "
                              "list_source_words levels:true first."),
                   false)
            .Field("unheard_margin_db",
                   IntSchema("How far above the source's noise floor "
                             "(floor_dbfs in the report) a kept word's peak "
                             "(peak_dbfs) may sit and still count as unheard; "
                             "0 to 30, default 3. On a real rush, unsaid words "
                             "peaked 5 to 8 dB above the floor."),
                   false)
            .Build("tighten_pauses arguments"),
        DispatchTightenPauses);

    // The speech groups are those of QC-2026-09b B1, which
    // analyze_speech_onset caches; PRD14 names the tool, not the ticket.
    add("trim_boundary_air",
        "Trim measured silence from the head and tail of one clip. Boundaries "
        "come from the speech groups analyze_speech_onset caches, never "
        "transcript word timestamps." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip whose boundaries to tighten."),
                   false)
            .Field("clip_ids",
                   IdArraySchema("Several clips instead of clip_id, in this "
                                 "order, one undo step each; one result per "
                                 "clip under `results`."),
                   false)
            .Field("keep_frames",
                   IntSchema("Source frames kept beside each speech attack "
                             "or decay (default 3)."),
                   false)
            .Field("min_air_ms",
                   IntSchema("Shortest boundary silence to close, in "
                             "milliseconds (default 300)."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema("Explicit tracks to ripple. When omitted, "
                                 "all other sync-locked tracks follow."),
                   false)
            .Build("trim_boundary_air arguments"),
        DispatchTrimBoundaryAir);

    add("close_junction_air",
        "Close the audible silence across one exact clip junction. Measures "
        "the outgoing tail and incoming head together, then trims both. Uses "
        "speech groups rather than word timestamps. Each side carries its own "
        "A/V partner (reported in also_cut), and the sync-locked tracks "
        "follow." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("left_clip_id", IdSchema("Outgoing clip."), false)
            .Field("right_clip_id", IdSchema("Incoming clip."), false)
            .Field("pairs",
                   ArraySchema(ArraySchema(IdSchema(""), ""),
                               "Several junctions as [left_clip_id, "
                               "right_clip_id] pairs, in this order, one undo "
                               "step each; one result per pair under "
                               "`results`. Instead of the two ids."),
                   false)
            .Field("keep_frames",
                   IntSchema("Source frames kept on each side (default 3)."),
                   false)
            .Field("min_air_ms",
                   IntSchema("Shortest combined junction air to close, in "
                             "milliseconds (default 300)."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema("Explicit tracks to ripple. When omitted, "
                                 "all other sync-locked tracks follow."),
                   false)
            .Build("close_junction_air arguments"),
        DispatchCloseJunctionAir);

    add("list_speech_onsets",
        "Report where the voice actually starts inside every audible clip, "
        "measured from the audio itself. Use it before trusting a cut's in "
        "point: the voice can start later than the words say. Each entry "
        "publishes the source-domain speech `groups` with their average and "
        "peak dBFS, plus `dominant_onset`: the first sustained group close to "
        "the clip's own level, so a quiet interviewer question is not mistaken "
        "for the subject's answer. `suggested_trim` in whole sequence frames, "
        "pre-roll already deducted: pass it straight to ripple_trim rather "
        "than computing one, and use `link_group_id` to trim the picture and "
        "sound together. Clips whose source has not been analysed are listed "
        "under \"unanalyzed\" and are not a pass: they are unknown.",
        SchemaBuilder()
            .Field("group_gap_ms",
                   IntSchema("Silence that separates two speech groups, in "
                             "milliseconds (default 200)."),
                   false)
            .Field("group_floor_db",
                   IntSchema("Level above the source noise floor required "
                             "for a speech group (default 6 dB)."),
                   false)
            .Field("dominant_percentile",
                   IntSchema("Clip level reference percentile (default 90)."),
                   false)
            .Field("dominance_db",
                   IntSchema("Maximum level difference from that percentile, "
                             "in dB (default 9)."),
                   false)
            .Field("dominant_sustain_windows",
                   IntSchema("Minimum group duration in 20 ms windows "
                             "(default 6)."),
                   false)
            .Build("list_speech_onsets arguments"),
        DispatchListSpeechOnsets);

    add("analyze_hesitations",
        "Find hesitations in one rush's sound, including those no transcript "
        "wrote: a detached \"euh\", a stretched syllable (\"la maa...rée\"), "
        "a murmur between words. Measured on timbre stability and level "
        "against the rush's speech; each candidate is placed against the "
        "aligned words, with its family, source range, measure and "
        "neighbouring words. A hold between two aligned words is listed from "
        "100 ms, one inside a word or on a rush without words from 200 ms, a "
        "murmur from 150 ms: shorter is a comma. Caches the result for "
        "list_disfluencies.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush to analyse."), false)
            .Field("media_ids",
                   IdArraySchema("Rushes to analyse in one call, in place "
                                 "of media_id: one line per rush under "
                                 "`results`, each with its own report or "
                                 "error."),
                   false)
            .Field("include_windows",
                   BoolSchema("Also return the per-10 ms stability and level "
                              "series; default false."),
                   false)
            .Build("analyze_hesitations arguments"),
        DispatchAnalyzeHesitations);

    add("analyze_speech_onset",
        "Measure one source's speech envelope and cache it, so "
        "list_speech_onsets has something to read. Decodes the whole audio; "
        "long-running.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Source media to analyse."), false)
            .Field("media_ids",
                   IdArraySchema("Media to analyse in one call, in place of "
                                 "media_id: one line per media under "
                                 "`results`, each with its own report or "
                                 "error."),
                   false)
            .Field("group_gap_ms",
                   IntSchema("Silence that separates two speech groups, in "
                             "milliseconds (default 200)."),
                   false)
            .Field("group_floor_db",
                   IntSchema("Level above the source noise floor required "
                             "for a speech group (default 6 dB)."),
                   false)
            .Build("analyze_speech_onset arguments"),
        DispatchAnalyzeSpeechOnset);

    add("list_source_words",
        "Read the aligned words of one rush with, for each, the frames to cut "
        "on. `start_frame`/`end_frame` (end exclusive) are the aligner's own, "
        "in the source's frames; `word_timing` says whose (mfa, nemo or "
        "whisper). `entry_frame`/`exit_frame` apply the interview skill's "
        "boundary rules to the source's measured envelope (`entry_rule` and "
        "`exit_rule` name which applied), and `voiced:false` marks a word the "
        "envelope does not hear (misaligned, not a place to cut). Without the "
        "envelope (analyze_speech_onset) the frames are the aligner's and the "
        "rules say no_envelope. Narrow with from_frame/to_frame, or `find` a "
        "word (ASCII case folded, accents as written) and get it with its "
        "neighbours; the answer is capped by max_words (default 200), then "
        "`truncated` and `next_from_frame` say how to continue. Reads only.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush whose words to read."), true)
            .Field("from_frame",
                   IntSchema("First source frame of interest; words ending "
                             "before it are left out."),
                   false)
            .Field("to_frame",
                   IntSchema("Source frame after the last of interest; "
                             "words starting at or after it are left out."),
                   false)
            .Field("find",
                   StringSchema("Show only the words containing this text, "
                                "each with context_words neighbours on "
                                "either side."),
                   false)
            .Field("context_words",
                   IntSchema("Neighbours shown around each match of `find`, "
                             "0 to 20; default 3."),
                   false)
            .Field("max_words",
                   IntSchema("Most words in one answer, 1 to 1000; default "
                             "200."),
                   false)
            .Field("levels",
                   BoolSchema("true: add each word's envelope in dBFS per "
                              "frame (`levels_dbfs`, from "
                              "`levels_from_frame`), three frames before "
                              "the entry to three after the exit, to see "
                              "the voice the rules read; with both "
                              "from_frame and to_frame, also `range_levels`, "
                              "one dBFS per frame over the whole range (at "
                              "most 1000) with the speech level and the "
                              "floor, to see a pause or a hesitation between "
                              "two words without reading the cache; default "
                              "false."),
                   false)
            .Build("list_source_words arguments"),
        DispatchListSourceWords);

    // `restarts` is AGENT-2026-09 A43 and `head_in_voice`/`tail_in_voice`
    // A48; PRD14 keeps those names here, out of the description. PRD15 --
    // Claude Code cuts a tool description past 2,048 characters, and this
    // one ran to 2,159: the same facts, said once each.
    add("list_timeline_words",
        "Read the montage's words without transcribing it: the rushes' aligned "
        "words (list_source_words' own, envelope rules included) laid on every "
        "clip of every audio track, as one flat `clips` list (tracks in index "
        "order, clips in time order). Per clip: its frames on the sequence and "
        "on its source, `words` with their source frames and the sequence "
        "frame each starts on (`whole:false` when the clip covers only part of "
        "the word, `voiced:false` when the envelope does not hear it), "
        "`head_cut`/`tail_cut` when a word straddles the in or the out (a cut "
        "into a word), `air` (silent frames from the in to the first voice and "
        "from the last voice to the out; `silent` for a clip with no voice at "
        "all) and `voice_without_words`, the sound no word covers -- these two "
        "when the source's envelope is cached (analyze_speech_onset). "
        "`hesitations` (the analyze_hesitations candidates the clip still "
        "plays: the \"euh\" whisper never writes) and `restarts` (a word said "
        "twice that the transcript wrote once). "
        "`head_in_voice`/`tail_in_voice`: a cut inside loud voice chops a "
        "syllable, even faded; move it to the silence or keep the word. `text` "
        "reads the whole montage, clips separated by ' | '; `summary` counts "
        "every check, hesitations_remaining, restarts_remaining and "
        "cuts_in_voice included. A hesitation or a cut in voice is heard; a "
        "restart is a candidate to check on its words before cutting, since "
        "the detector also marks whole long words. Holds under three frames "
        "are remnants (remnant:true), listed but not counted. Run it after "
        "every cut; transcribe_timeline hears what plays at the joins. A "
        "source without a cached transcript leaves its clips unchecked "
        "(unchecked_sources). Reads only.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Only these audio clips; every audio clip "
                                 "of the timeline when omitted."),
                   false)
            .Field("text_only",
                   BoolSchema("true: only `text`, `summary` and "
                              "unchecked_sources, no per-clip detail; "
                              "default false."),
                   false)
            .Build("list_timeline_words arguments"),
        DispatchListTimelineWords);

    // `voice_end_frame` is AGENT-2026-09 A48; PRD14 keeps the ticket here.
    add("list_disfluencies",
        "List the fillers (euh, heu, hum, ben) and stuttered repetitions found "
        "in one clip's cached transcript, as word index ranges ready for "
        "remove_words. Detection is deterministic, not a judgement call: "
        "review the text of each entry, drop the ones that carry meaning, and "
        "pass the rest through. Kind Marker names a discourse marker (donc, "
        "enfin, voil\u00e0, bref, genre, en fait, en gros, du coup, tu vois) "
        "with its word indices, for remove_words. `restarts` lists, with the "
        "source's envelope cached, every aligned word long enough to hold two "
        "utterances with a hollow between two stretches of voice: a word said "
        "twice that the transcript wrote once (\"l'Horloge... l'Horloge\"), "
        "whose suggested_cut removes the first utterance whole so the second "
        "starts clean. The detector also marks whole long words (a stop "
        "consonant leaves the same hollow): check each restart on the words "
        "(list_timeline_words) before cutting it -- a short word, or a "
        "repetition the text shows. `hesitations` lists the "
        "analyze_hesitations candidates inside the clip with the range a cut "
        "takes: `suggested_cut` runs from the hold to the end of the voice "
        "that follows it, up to the next aligned word (`voice_end_frame`: "
        "stopping at the hold's end leaves a decay heard as a \"euh\"), never "
        "into a neighbouring word (clamped_to_words). `words_inside` names an "
        "aligned word wholly inside the hold (often the hold's own vowel) and "
        "`cut_including_words_inside` the cut that takes it along, for when "
        "the sentence survives without it.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip whose transcript to inspect."),
                   true)
            .Field("force",
                   BoolSchema("Override the refusal of a transcript with "
                              "likely_hallucinated=true. Defaults to false."),
                   false)
            .Build("list_disfluencies arguments"),
        DispatchListDisfluencies);

    const std::string wordRangeSchema =
        "{\"type\":\"object\",\"properties\":{\"start_word_index\":" +
        IntSchema("First word to remove, inclusive.") +
        ",\"end_word_index\":" + IntSchema("Last word to remove, inclusive.") +
        "},\"required\":[\"start_word_index\",\"end_word_index\"],"
        "\"additionalProperties\":false}";

    add("remove_words",
        "Remove word ranges from one clip and ripple-close every cut. Word "
        "indices come from list_disfluencies." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to cut."), true)
            .Field("ranges",
                   ArraySchema(wordRangeSchema,
                               "Sorted, non-overlapping word index ranges."),
                   true)
            .Field("force",
                   BoolSchema("Override the refusal of a transcript with "
                              "likely_hallucinated=true. Defaults to false."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema(
                       "Tracks that follow the cut beyond the linked pair; "
                       "default: every unlocked sync-locked track (a cutaway "
                       "over removed words shortens with them)."),
                   false)
            .Build("remove_words arguments"),
        DispatchRemoveWords);

    add("remove_source_ranges",
        "Remove ranges of the clip's own rush, named in that rush's frames "
        "[first_frame, end_frame), and ripple-close every cut. This is how a "
        "hesitation no transcript wrote is cut: take suggested_cut from "
        "list_disfluencies' hesitations." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to cut."), true)
            .Field("ranges",
                   ArraySchema(SchemaBuilder()
                                   .Field("first_frame",
                                          IntSchema("First source frame "
                                                    "removed."),
                                          true)
                                   .Field("end_frame",
                                          IntSchema("One past the last source "
                                                    "frame removed."),
                                          true)
                                   .Build("source frame range"),
                               "Source frame ranges to remove."),
                   true)
            .Field("sync_track_ids",
                   IdArraySchema("Tracks that follow the cut beyond the linked "
                                 "pair, which always does; every other "
                                 "unlocked sync-locked track when omitted."),
                   false)
            .Build("remove_source_ranges arguments"),
        DispatchRemoveSourceRanges);

    add("align_transcript",
        "The fallback for a rush align_words could not align: pull every "
        "cached Whisper transcript's word boundaries onto the speech envelope, "
        "and report which ones the signal contradicts. Where MFA aligned a "
        "rush, the word-level tools read MFA's boundaries, not these. "
        "Read-only unless apply is true, which installs the corrections in the "
        "transcript cache the word-level tools read. Requires a cached "
        "transcript and a cached speech onset analysis per source.",
        SchemaBuilder()
            .Field("apply",
                   BoolSchema("Write the corrected boundaries back to the "
                              "transcript cache (default false)."),
                   false)
            .Build("align_transcript arguments"),
        DispatchAlignTranscript);

    add("align_words",
        "Time every word of the rushes with Montreal Forced Aligner: Whisper "
        "gives the text, MFA the boundaries. Every word-level tool reads them "
        "from then on (word_timing \"mfa\"). The answer lists per rush the "
        "words the engine now reads and who timed them, what was skipped and "
        "why (not_aligned: MFA could not align the rush, which keeps its "
        "previous words), and uncovered_speech: stretches of speech no Whisper "
        "word covers, in source frames. MFA aligns only the text it is given, "
        "so each of those drags the neighbouring words into it: listen, then "
        "run again with `texts` giving that rush's full text.",
        SchemaBuilder()
            .Field("media_ids",
                   IdArraySchema("Rushes to align; every rush with a cached "
                                 "transcript when omitted."),
                   false)
            .Field("texts",
                   R"({"type":"object","additionalProperties":{"type":)"
                   R"("string"},"description":"media_id -> the rush's )"
                   R"(whole text, replacing Whisper's where speech has )"
                   R"(no words (uncovered_speech)."})",
                   false)
            .Build("align_words arguments"),
        DispatchAlignWords);

    add("analyze_speakers",
        "Tell the voices of one rush apart: who speaks when. `speakers` is "
        "how many voices the rush holds, which you know and the model does "
        "not: it groups, it does not count (1 marks every stretch as the one "
        "voice). A speaker embedding model reads the rush's sound on this "
        "machine; nothing leaves it. Transcribe the rush first: the stretches "
        "of speech are then its words, and a voice changes in a pause. The "
        "voices are numbers, 0, 1, 2 in order of first appearance, of this "
        "rush only: read list_speakers to tell who each is. Its `similarity` "
        "says how alike two voices are, per mille: told apart, voices "
        "measured 380 to 800; from 850 up the two are one voice cut in two "
        "or two it cannot tell apart, and the split is not to be relied on. "
        "Measured on spliced French dialogues: about 6 % of the time goes to "
        "the wrong voice, 25 % on two very close voices. Then "
        "list_source_words gives each word its `speaker`.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush to read."), true)
            .Field("speakers",
                   IntSchema("How many voices the rush holds, 1 to 8."), true)
            .Build("analyze_speakers arguments"),
        DispatchAnalyzeSpeakers);

    add("list_speakers",
        "Read what analyze_speakers found in a rush: for each voice how long "
        "it speaks and in how many turns, how alike two voices are "
        "(`similarity`, per mille; from 850 up, not to be relied on), and the "
        "turns in order, each with its voice, its source times and its first "
        "words. A voice is a number: who it is, you read from the words. To "
        "cut a multicam by the voice, split the group at the turns' times "
        "(split_multicam_group) and give each piece its angle "
        "(set_multicam_active_angle).",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush analyze_speakers has read."),
                   true)
            .Field("max_turns",
                   IntSchema("Turns at most, 1 to 500 (default 100)."), false)
            .Field("from_turn",
                   IntSchema("First turn to list (next_from_turn of a "
                             "previous answer)."),
                   false)
            .Build("list_speakers arguments"),
        DispatchListSpeakers);

    add("transcribe_media",
        "Transcribe a source's audio locally and cache the transcript, so the "
        "word-level tools have something to read. Every tool that names words "
        "needs it once per media. Runs a local model with no network access. "
        "Media the document records as mute are skipped and reported rather "
        "than transcribed. Each result carries `text`, the words of the "
        "transcript joined (the first 6000 characters, `text_truncated` past "
        "that): read the rushes here, not from the cache file `path` names; "
        "list_source_words gives the words with their frames. A verbatim pass "
        "found likely_incomplete (analyze_speech_onset first) is redone plain, "
        "keeping its hesitations where the plain pass heard nothing "
        "(`plain_pass`); speech that came out as \"...\" is decoded again "
        "with the ellipsis forbidden and its words take the ellipses' place "
        "(`ellipsis_pass`).",
        SchemaBuilder()
            .Field("media_id", IdSchema("One media to transcribe."), false)
            .Field("media_ids",
                   IdArraySchema("Several media to transcribe in one batch, "
                                 "sharing a single model load."),
                   false)
            .Field("include_silent",
                   BoolSchema("Transcribe even media whose measured audio "
                              "level says they carry no speech (default "
                              "false)."),
                   false)
            .Field("language",
                   StringSchema("ISO 639-1 code such as \"fr\", or \"auto\" "
                                "to detect it. Defaults to \"auto\"."),
                   false)
            .Field("decoding_profile",
                   EnumSchema({"default", "fr-hesitations-v1"},
                              "default (unchanged), or fr-hesitations-v1: "
                              "experimental euh decoding for the evaluated "
                              "s075 model only; requires language=fr and "
                              "verbatim=true."),
                   false)
            .Field("verbatim",
                   BoolSchema("Keep hesitations and filler words. Required if "
                              "they are to be removable afterwards."),
                   false)
            // PRD40 -- ROADMAP.md. A run that looks worse than the cached
            // transcript leaves the cache alone (`cache_kept`).
            .Field("overwrite",
                   BoolSchema("Replace the cached transcript even when this "
                              "one looks worse (flagged likely_incomplete or "
                              "likely_hallucinated, or under half its words): "
                              "without it the cache is kept and the result "
                              "says so with cache_kept."),
                   false)
            .Build("transcribe_media arguments"),
        DispatchTranscribeMedia);

    add("transcribe_timeline",
        "Transcribe exactly the audible audio assembled by a timeline: each "
        "audio clip is decoded at its source bounds, placed at its exact "
        "timeline position and mixed directly into Whisper. The cache is "
        "invalidated when an audible clip boundary changes.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Timeline ID; omit for the active timeline."),
                   false)
            .Field("language",
                   StringSchema("ISO 639-1 code such as \"fr\", or \"auto\" "
                                "to detect it. Defaults to \"auto\"."),
                   false)
            .Field("decoding_profile",
                   EnumSchema({"default", "fr-hesitations-v1"},
                              "default (unchanged), or fr-hesitations-v1: "
                              "experimental euh decoding for the evaluated "
                              "s075 model only; requires language=fr and "
                              "verbatim=true."),
                   false)
            .Field("verbatim", BoolSchema("Keep hesitations and filler words."),
                   false)
            .Build("transcribe_timeline arguments"),
        DispatchTranscribeTimeline);
}
