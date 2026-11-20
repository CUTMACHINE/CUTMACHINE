// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// sound: offsets, ducking and sync, written sound, beats, tempo, music
// structure and the timeline's audio. Moved out of McpTools.cc as they were;
// what they share with the other files of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// SYNCHRO-2026-10 SY1 -- docs/roadmap/SYNCHRO-2026-10.md. Read only: how
// far one rush's sound sits from another's (AudioOffset.h). A refusal keeps
// the measure's own name (NoOverlap, WeakCorrelation, AmbiguousPeak,
// ClockDrift), never a guessed value.
bool DispatchMeasureAudioOffset(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "measure_audio_offset";
    if (!CheckKnownKeys(
            args,
            {"reference_id", "other_id", "reference_channel", "other_channel"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    Ulid referenceId;
    Ulid otherId;
    std::string referenceChannel;
    std::string otherChannel;
    if (!ReadId(args, "reference_id", tool, resolver, true, referenceId,
                message) ||
        !ReadId(args, "other_id", tool, resolver, true, otherId, message) ||
        !ReadString(args, "reference_channel", tool, false, "mix",
                    referenceChannel, message) ||
        !ReadString(args, "other_channel", tool, false, "mix", otherChannel,
                    message)) {
        return Fail(errorName, message, message);
    }
    AudioOffsetChannels channels;
    if (!ParseAudioOffsetChannel(referenceChannel, channels.reference) ||
        !ParseAudioOffsetChannel(otherChannel, channels.other)) {
        return Fail(errorName, message,
                    "'" + tool + "' channels are mix, left or right");
    }
    return backend.MeasureAudioOffset(referenceId, otherId, channels,
                                      resultJson, errorName, message);
}

// FC6 -- docs/roadmap/FILMCRAFT-2026-10.md. Read only: where one rush's
// voice speaks and the gain curve a music under it would follow
// (AudioDspDucking.h). Nothing is written: the clip gain cannot be animated
// yet (REEL-2026-09 R2), so the curve is a proposal for whoever mixes.
bool DispatchProposeDucking(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    const std::string tool = "propose_ducking";
    if (!CheckKnownKeys(args,
                        {"media_id", "channel", "threshold_db", "hysteresis_db",
                         "floor_percentile", "minimum_speech_ms", "bridge_ms",
                         "depth_db", "attack_ms", "release_ms"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    Ulid mediaId;
    std::string channelName;
    audio_dsp::DuckingSettings settings;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadString(args, "channel", tool, false, "mix", channelName,
                    message)) {
        return Fail(errorName, message, message);
    }
    const std::vector<std::pair<const char*, int64_t*>> numbers = {
        {"threshold_db", &settings.threshold_db},
        {"hysteresis_db", &settings.hysteresis_db},
        {"floor_percentile", &settings.floor_percentile},
        {"minimum_speech_ms", &settings.minimum_speech_milliseconds},
        {"bridge_ms", &settings.bridge_milliseconds},
        {"depth_db", &settings.depth_db},
        {"attack_ms", &settings.attack_milliseconds},
        {"release_ms", &settings.release_milliseconds}};
    for (const auto& [key, field] : numbers) {
        if (!ReadInt64(args, key, tool, false, *field, *field, message)) {
            return Fail(errorName, message, message);
        }
    }
    AudioOffsetChannel channel = AudioOffsetChannel::Mix;
    if (!ParseAudioOffsetChannel(channelName, channel)) {
        return Fail(errorName, message,
                    "'" + tool + "' channel is mix, left or right");
    }
    // Refused here, before a task decodes the whole rush to say the same.
    std::string invalid;
    if (!audio_dsp::ValidateDuckingSettings(
            settings, audio_dsp::kDuckingSampleRate, invalid)) {
        return Fail(errorName, message, "'" + tool + "' " + invalid);
    }
    return backend.ProposeDucking(mediaId, channel, settings, resultJson,
                                  errorName, message);
}

// SYNCHRO-2026-10 SY2 -- docs/roadmap/SYNCHRO-2026-10.md. A recorder's sound
// laid under a picture clip, linked, in one undo (SeparateSound.h). The
// offset is measured as measure_audio_offset measures it, between the
// picture's own rush and the recorder, unless the caller hands it over from
// such a measure; a refusal of the measure is the tool's refusal.
//
// SY2, the window -- with sound_clip_id instead of media_id and track_id,
// the recorder's sound is a clip already on the timeline, and it is slid
// into sync on its own track (BuildSeparateSoundAlign): Resolve's Auto Align
// Clips, Based on Waveform, which the window's gesture applies too.
bool DispatchSyncAudio(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    const std::string tool = "sync_audio";
    if (!CheckKnownKeys(
            args,
            {"clip_id", "media_id", "track_id", "sound_clip_id", "offset",
             "picture_channel", "recorder_channel", "mute_camera_sound"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    const bool aligning = args.Find("sound_clip_id") != nullptr;
    if (aligning && (args.Find("media_id") != nullptr ||
                     args.Find("track_id") != nullptr)) {
        return Fail(errorName, message,
                    "'" + tool +
                        "' takes either sound_clip_id, a sound already on "
                        "the timeline, or media_id and track_id, not both");
    }
    SeparateSoundRequest request;
    Ulid soundClipId;
    std::string pictureChannel;
    std::string recorderChannel;
    if (!ReadId(args, "clip_id", tool, resolver, true, request.picture_clip_id,
                message)) {
        return Fail(errorName, message, message);
    }
    if (!aligning && (args.Find("media_id") == nullptr ||
                      args.Find("track_id") == nullptr)) {
        return Fail(errorName, message,
                    "'" + tool +
                        "' takes media_id and track_id, or instead "
                        "sound_clip_id, a sound already on the timeline");
    }
    if (!ReadId(args, "media_id", tool, resolver, false,
                request.recorder_source_id, message) ||
        !ReadId(args, "track_id", tool, resolver, false, request.track_id,
                message) ||
        !ReadId(args, "sound_clip_id", tool, resolver, false, soundClipId,
                message) ||
        !ReadString(args, "picture_channel", tool, false, "mix", pictureChannel,
                    message) ||
        !ReadString(args, "recorder_channel", tool, false, "mix",
                    recorderChannel, message) ||
        !ReadBool(args, "mute_camera_sound", tool, true,
                  request.mute_camera_sound, message)) {
        return Fail(errorName, message, message);
    }
    AudioOffsetChannels channels;
    if (!ParseAudioOffsetChannel(pictureChannel, channels.reference) ||
        !ParseAudioOffsetChannel(recorderChannel, channels.other)) {
        return Fail(errorName, message,
                    "'" + tool + "' channels are mix, left or right");
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* picture = document.FindClip(request.picture_clip_id);
    if (picture == nullptr) {
        errorName = "UnknownClip";
        message = "unknown clip_id '" + request.picture_clip_id + "'";
        return false;
    }
    if (aligning) {
        // Refused by name before any sound is decoded: the builder says
        // the pair's fault in its own words.
        if (CheckSeparateSoundPair(document, request.picture_clip_id,
                                   soundClipId) !=
            SeparateSoundPairFault::None) {
            BatchOperation unused;
            SeparateSoundPlacement none;
            EditError editError = EditError::None;
            BuildSeparateSoundAlign(
                document, {request.picture_clip_id, soundClipId, {0, 1}, true},
                unused, none, editError, message);
            errorName = EditErrorName(editError);
            return false;
        }
        request.recorder_source_id = document.FindClip(soundClipId)->source_id;
    }
    Value measure;
    if (args.Find("offset") != nullptr) {
        if (!ReadTime(args, "offset", tool, true, request.offset, message)) {
            return Fail(errorName, message, message);
        }
    } else {
        std::string measureJson;
        if (!backend.MeasureAudioOffset(picture->source_id,
                                        request.recorder_source_id, channels,
                                        measureJson, errorName, message)) {
            return false;
        }
        std::string parseError;
        if (!Value::Parse(measureJson, measure, parseError) ||
            !ReadTime(measure, "offset", tool, true, request.offset, message)) {
            errorName = "IoError";
            message = "the measure gave no offset: " + measureJson;
            return false;
        }
    }
    BatchOperation batch;
    SeparateSoundPlacement placement;
    EditError editError = EditError::None;
    const bool built =
        aligning ? BuildSeparateSoundAlign(
                       document,
                       {request.picture_clip_id, soundClipId, request.offset,
                        request.mute_camera_sound},
                       batch, placement, editError, message)
                 : BuildSeparateSoundSync(document, request, batch, placement,
                                          editError, message);
    if (!built) {
        errorName = EditErrorName(editError);
        return false;
    }
    if (batch.operations.empty()) {
        // Already in sync, linked, the camera's track off: nothing written.
        resultJson = "{\"ok\":true,\"unchanged\":true}";
    } else if (!backend.ApplyOperation(batch, resultJson, errorName, message)) {
        return false;
    }
    Value result;
    std::string parseError;
    if (Value::Parse(resultJson, result, parseError) && result.IsObject()) {
        const auto TimeValue = [](const RationalTime& time) {
            Value value = Value::MakeObject();
            value.Set("value", Value::MakeInt(time.value));
            value.Set("rate", Value::MakeInt(time.rate));
            return value;
        };
        Value placed = Value::MakeObject();
        placed.Set("clip_id", Value::MakeString(placement.clip_id));
        placed.Set("source_in", TimeValue(placement.source_in));
        placed.Set("duration", TimeValue(placement.duration));
        placed.Set("timeline_in", TimeValue(placement.timeline_in));
        placed.Set("muted_track_id",
                   placement.muted_track_id.empty()
                       ? Value::MakeNull()
                       : Value::MakeString(placement.muted_track_id));
        result.Set("placed", std::move(placed));
        if (measure.IsObject()) {
            result.Set("measure", std::move(measure));
        }
        resultJson = result.Dump();
    }
    return true;
}

// SN7 -- docs/roadmap/SON-2026-10.md. A ditty rendered on the cuts of the
// timeline as it is now, by cutmachine-sound-host (SoundRender.h): the
// report `cm-sound render` prints, and a stem ready for place_sound_stem.
bool DispatchRenderSound(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "render_sound";
    if (!CheckKnownKeys(args,
                        {"ditty", "partition", "seed", "settings",
                         "sample_rate", "tail", "track_id", "start_timecode",
                         "out", "stems", "triggers", "timeout"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    SoundRenderRequest request;
    int64_t sampleRate = 48000;
    int64_t timeout = 300;
    bool stems = false;
    std::string out;
    if (!ReadString(args, "ditty", tool, true, "", request.ditty_path,
                    message) ||
        !ReadString(args, "partition", tool, false, "", request.partition_path,
                    message) ||
        !ReadInt64(args, "seed", tool, false, 1, request.seed, message) ||
        !ReadInt64(args, "sample_rate", tool, false, 48000, sampleRate,
                   message) ||
        !ReadId(args, "track_id", tool, resolver, false, request.track_id,
                message) ||
        !ReadString(args, "start_timecode", tool, false, "",
                    request.start_timecode, message) ||
        !ReadString(args, "out", tool, false, "", out, message) ||
        !ReadBool(args, "stems", tool, false, stems, message) ||
        !ReadBool(args, "triggers", tool, false, request.triggers, message) ||
        !ReadInt64(args, "timeout", tool, false, 300, timeout, message)) {
        return Fail(errorName, message, message);
    }
    if (sampleRate < 8000 || sampleRate > 384000) {
        return Fail(errorName, message,
                    "'" + tool + ".sample_rate' must be 8000 to 384000");
    }
    if (timeout < 1 || timeout > 3600) {
        return Fail(errorName, message,
                    "'" + tool + ".timeout' must be 1 to 3600 seconds");
    }
    request.sample_rate = static_cast<int32_t>(sampleRate);
    request.timeout_seconds = static_cast<int>(timeout);
    if (const Value* settings = args.Find("settings")) {
        if (!settings->IsObject()) {
            return Fail(errorName, message,
                        "'" + tool + ".settings' must be an object");
        }
        request.settings = *settings;
    }
    if (const Value* tail = args.Find("tail")) {
        if (!tail->IsNumber()) {
            return Fail(errorName, message,
                        "'" + tool + ".tail' must be a number of seconds");
        }
        request.tail = *tail;
    }
    // Without `out`, the stem goes in the package's Sound folder, named as
    // the sidecar names it (sidecar/sound.py).
    if (out.empty()) {
        const std::string package = backend.PackageDirectory();
        if (package.empty()) {
            return Fail(errorName, message,
                        "'" + tool +
                            "' needs `out` when no project package is open: "
                            "the stem has nowhere else to go");
        }
        std::string name =
            std::filesystem::path(request.ditty_path).stem().string();
        if (!request.partition_path.empty()) {
            name =
                std::filesystem::path(request.partition_path).stem().string() +
                "-" + name;
        }
        out = (std::filesystem::path(package) / "Sound" /
               (name + "-seed" + std::to_string(request.seed) + ".wav"))
                  .string();
    }
    request.out_path =
        std::filesystem::absolute(out).lexically_normal().string();
    if (stems) {
        std::filesystem::path folder(request.out_path);
        folder.replace_extension();
        request.stems_dir = folder.string() + "-stems";
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return RenderSound(document, request, resultJson, errorName, message);
}

// SN4 -- docs/roadmap/SON-2026-10.md. A stem cutmachine-sound rendered,
// already in the library, laid where its BWF timecode says (SoundStem.h):
// the file is read here, through the library's path, so that the agent
// names a media and never a time.
bool DispatchPlaceSoundStem(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    const std::string tool = "place_sound_stem";
    if (!CheckKnownKeys(args, {"media_id", "track_id", "start_timecode"}, tool,
                        message)) {
        return Fail(errorName, message, message);
    }
    SoundStemRequest request;
    if (!ReadId(args, "media_id", tool, resolver, true, request.media_id,
                message) ||
        !ReadId(args, "track_id", tool, resolver, false, request.track_id,
                message) ||
        !ReadString(args, "start_timecode", tool, false,
                    kEdlDefaultStartTimecode, request.start_timecode,
                    message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(request.media_id);
    if (media == nullptr) {
        errorName = "UnknownMedia";
        message = "unknown media_id '" + request.media_id + "'";
        return false;
    }
    // A library path is relative to the package (Ingest.h, StoredMediaPath).
    std::filesystem::path path(media->path);
    const std::string package = backend.PackageDirectory();
    if (path.is_relative() && !package.empty()) path = package / path;
    // A rush with a picture is refused by the builder, before its file is
    // read as a WAV it is not.
    std::string readError;
    if (!media->has_video &&
        !ReadBroadcastWave(path.lexically_normal().string(), request.wave,
                           readError)) {
        errorName = "IoError";
        message = readError;
        return false;
    }
    BatchOperation batch;
    SoundStemPlacement placement;
    EditError editError = EditError::None;
    if (!BuildSoundStemPlacement(document, request, batch, placement, editError,
                                 message)) {
        errorName = EditErrorName(editError);
        return false;
    }
    if (!backend.ApplyOperation(batch, resultJson, errorName, message)) {
        return false;
    }
    Value result;
    std::string parseError;
    if (Value::Parse(resultJson, result, parseError) && result.IsObject()) {
        const auto TimeValue = [](const RationalTime& time) {
            Value value = Value::MakeObject();
            value.Set("value", Value::MakeInt(time.value));
            value.Set("rate", Value::MakeInt(time.rate));
            return value;
        };
        Value placed = Value::MakeObject();
        placed.Set("clip_id", Value::MakeString(placement.clip_id));
        placed.Set("track_id", Value::MakeString(placement.track_id));
        placed.Set("track_added", Value::MakeBool(placement.track_added));
        placed.Set("timeline_in", TimeValue(placement.timeline_in));
        placed.Set("duration", TimeValue(placement.duration));
        placed.Set("first_frame", Value::MakeInt(placement.first_frame));
        placed.Set("frames", Value::MakeInt(placement.frames));
        placed.Set("stem_duration", TimeValue(placement.stem_duration));
        placed.Set("not_laid", TimeValue(placement.not_laid));
        placed.Set("time_reference",
                   Value::MakeInt(request.wave.time_reference));
        placed.Set("sample_rate", Value::MakeInt(request.wave.sample_rate));
        placed.Set("start_timecode", Value::MakeString(request.start_timecode));
        result.Set("placed", std::move(placed));
        resultJson = result.Dump();
    }
    return true;
}

// F1.6 -- the source audio analysis is a cached measurement. A model can
// select which positions matter, but neither audio decode nor arithmetic
// belongs in the model's tool arguments.
bool ResolveAudioBeatMedia(McpBackend& backend, const Ulid& mediaId,
                           Document& document, std::string& errorName,
                           std::string& message) {
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentSource* source = document.FindSource(mediaId);
    const auto media = std::find_if(
        document.library.begin(), document.library.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    if (source == nullptr || media == document.library.end()) {
        errorName = EditErrorName(EditError::UnknownMedia);
        message = "unknown media_id '" + mediaId + "'";
        return false;
    }
    if (media->metadata_complete && !media->has_audio) {
        errorName = EditErrorName(EditError::InvalidOperation);
        message =
            "media_id '" + mediaId + "' has no audio stream for beat analysis";
        return false;
    }
    return true;
}

Value BeatNumberValue(double number, int precision) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(precision) << number;
    return Value::MakeRawNumber(stream.str());
}

bool DispatchAnalyzeAudioBeats(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id"};
    if (!CheckKnownKeys(args, kAllowed, "analyze_audio_beats", message)) {
        return Fail(errorName, message, message);
    }
    Ulid mediaId;
    if (!ReadId(args, "media_id", "analyze_audio_beats", resolver, true,
                mediaId, message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!ResolveAudioBeatMedia(backend, mediaId, document, errorName,
                               message)) {
        return false;
    }
    return backend.AnalyzeSourceAudioBeats(mediaId, resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

bool DispatchListAudioBeats(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id", "source_in", "source_out", "max_onsets",
        "minimum_strength_percent"};
    if (!CheckKnownKeys(args, kAllowed, "list_audio_beats", message)) {
        return Fail(errorName, message, message);
    }
    Ulid mediaId;
    int32_t maxOnsets = 128;
    int32_t minimumStrength = 0;
    if (!ReadId(args, "media_id", "list_audio_beats", resolver, true, mediaId,
                message) ||
        !ReadInt32(args, "max_onsets", "list_audio_beats", false, maxOnsets,
                   maxOnsets, message) ||
        !ReadInt32(args, "minimum_strength_percent", "list_audio_beats", false,
                   minimumStrength, minimumStrength, message)) {
        return Fail(errorName, message, message);
    }
    if (maxOnsets < 1 || maxOnsets > 512 || minimumStrength < 0 ||
        minimumStrength > 100) {
        return Fail(errorName, message,
                    "'list_audio_beats.max_onsets' must be 1..512 and "
                    "minimum_strength_percent must be 0..100");
    }
    Document document;
    if (!ResolveAudioBeatMedia(backend, mediaId, document, errorName,
                               message)) {
        return false;
    }
    const DocumentSource* source = document.FindSource(mediaId);
    RationalTime sourceIn{0, 1};
    RationalTime sourceOut = source->duration;
    if (!ReadTime(args, "source_in", "list_audio_beats", false, sourceIn,
                  message) ||
        !ReadTime(args, "source_out", "list_audio_beats", false, sourceOut,
                  message)) {
        return Fail(errorName, message, message);
    }
    if (sourceIn.value < 0 || sourceOut.value < 0 || !(sourceIn < sourceOut) ||
        sourceOut > source->duration) {
        return Fail(errorName, message,
                    "'list_audio_beats' requires 0 <= source_in < "
                    "source_out <= media duration");
    }

    BeatDetectionResult beats;
    if (!backend.ReadSourceAudioBeats(mediaId, beats, message)) {
        return Fail(errorName, message, message);
    }
    Value response = Value::MakeObject();
    response.Set("media_id", Value::MakeString(mediaId));
    response.Set("source_in", BeatTimeValue(sourceIn));
    response.Set("source_out", BeatTimeValue(sourceOut));
    response.Set("total_onsets",
                 Value::MakeInt(static_cast<int64_t>(beats.onsets.size())));
    response.Set("tempo_scope", Value::MakeString("whole_source"));
    response.Set("bpm", beats.bpm.has_value() ? BeatNumberValue(*beats.bpm, 17)
                                              : Value::MakeNull());
    response.Set("bpm_confidence", BeatNumberValue(beats.bpm_confidence, 17));
    Value selected = Value::MakeArray();
    int64_t matching = 0;
    bool truncated = false;
    RationalTime nextSourceIn{0, 1};
    for (const AudioOnset& onset : beats.onsets) {
        if (onset.time < sourceIn || !(onset.time < sourceOut) ||
            static_cast<double>(onset.strength) * 100.0 < minimumStrength) {
            continue;
        }
        ++matching;
        if (selected.AsArray().size() >= static_cast<size_t>(maxOnsets)) {
            if (!truncated) {
                nextSourceIn = onset.time;
            }
            truncated = true;
            continue;
        }
        Value item = Value::MakeObject();
        item.Set("source_time", BeatTimeValue(onset.time));
        item.Set("strength", BeatNumberValue(onset.strength, 9));
        selected.Push(std::move(item));
    }
    response.Set("matching_onsets", Value::MakeInt(matching));
    response.Set(
        "returned_onsets",
        Value::MakeInt(static_cast<int64_t>(selected.AsArray().size())));
    response.Set("onsets", std::move(selected));
    response.Set("truncated", Value::MakeBool(truncated));
    if (truncated) {
        response.Set("next_source_in", BeatTimeValue(nextSourceIn));
    }
    resultJson = response.Dump();
    return true;
}

// R7 -- docs/roadmap/REEL-2026-09.md. The tempo grid is a read: it refits a
// period and a phase onto the onsets analyze_audio_beats already cached, and
// never decodes audio, never detects an onset and never edits a project. The
// engine does not guess where a bar starts either -- the author declares the
// origin and the bar length, and only then are bars numbered.
double TempoMilliseconds(int64_t samples, int32_t rate) {
    return rate > 0 ? 1000.0 * static_cast<double>(samples) / rate : 0.0;
}

bool ReadTempoBarSettings(const Value& args, const std::string& tool,
                          TempoBarSettings& bars, std::string& message) {
    int32_t beatsPerBar = 0;
    if (!ReadInt32(args, "beats_per_bar", tool, false, 0, beatsPerBar,
                   message)) {
        return false;
    }
    if (beatsPerBar < 0 || beatsPerBar > 32) {
        message = "'" + tool + ".beats_per_bar' must be 1..32";
        return false;
    }
    bars.beats_per_bar = beatsPerBar;
    if (args.Find("bar_origin") != nullptr) {
        RationalTime declared{0, 1};
        if (!ReadTime(args, "bar_origin", tool, false, declared, message)) {
            return false;
        }
        if (declared.value < 0) {
            message = "'" + tool + ".bar_origin' must not be negative";
            return false;
        }
        bars.has_origin = true;
        bars.origin = declared;
    }
    return true;
}

Value TempoResidualsValue(const TempoGrid& grid) {
    Value residuals = Value::MakeObject();
    residuals.Set("total_beats", Value::MakeInt(grid.residuals.total_beats));
    residuals.Set("matched_beats",
                  Value::MakeInt(grid.residuals.matched_beats));
    residuals.Set("median_absolute_samples",
                  Value::MakeInt(grid.residuals.median_absolute_samples));
    residuals.Set("median_absolute_milliseconds",
                  BeatNumberValue(
                      TempoMilliseconds(grid.residuals.median_absolute_samples,
                                        grid.sample_rate),
                      9));
    residuals.Set("percentile90_absolute_samples",
                  Value::MakeInt(grid.residuals.percentile90_absolute_samples));
    residuals.Set("maximum_absolute_samples",
                  Value::MakeInt(grid.residuals.maximum_absolute_samples));
    residuals.Set("maximum_absolute_milliseconds",
                  BeatNumberValue(
                      TempoMilliseconds(grid.residuals.maximum_absolute_samples,
                                        grid.sample_rate),
                      9));
    return residuals;
}

void AppendTempoGridFields(Value& response, const TempoGrid& grid) {
    response.Set("status", Value::MakeString(TempoGridStatusName(grid.status)));
    response.Set("accepted", Value::MakeBool(grid.accepted()));
    if (!grid.accepted()) {
        response.Set("refusal", Value::MakeString(grid.refusal));
    }
    response.Set("sample_rate", Value::MakeInt(grid.sample_rate));
    response.Set("period_samples", Value::MakeInt(grid.period_samples));
    response.Set("phase_samples", Value::MakeInt(grid.phase_samples));
    response.Set("bpm", grid.period_samples > 0
                            ? BeatNumberValue(grid.bpm(), 17)
                            : Value::MakeNull());
    response.Set("coherence", BeatNumberValue(grid.coherence, 9));
    response.Set("subdivision", Value::MakeInt(grid.subdivision));
    response.Set("beat_support_percent",
                 Value::MakeInt(grid.beat_support_percent));
    response.Set("subdivision_support_percent",
                 Value::MakeInt(grid.subdivision_support_percent));
    response.Set("candidate_period_samples",
                 Value::MakeInt(grid.candidate_period_samples));
    response.Set("doublings", Value::MakeInt(grid.doublings));
    response.Set("halvings", Value::MakeInt(grid.halvings));
    response.Set("beats_per_bar", Value::MakeInt(grid.beats_per_bar));
    Value origin = Value::MakeObject();
    origin.Set("declared", Value::MakeBool(grid.has_origin));
    origin.Set("beat_index", Value::MakeInt(grid.origin_index));
    origin.Set("snap_samples", Value::MakeInt(grid.origin_snap_samples));
    response.Set("origin", std::move(origin));
    response.Set("residuals", TempoResidualsValue(grid));
}

// Shared by both tempo reads: resolve the media, read its cached onsets and
// fit the grid over the requested window.
bool FitTempoGridForMedia(McpBackend& backend, const std::string& tool,
                          const Value& args, const Ulid& mediaId,
                          Document& document, RationalTime& sourceIn,
                          RationalTime& sourceOut, TempoGrid& grid,
                          std::string& errorName, std::string& message) {
    if (!ResolveAudioBeatMedia(backend, mediaId, document, errorName,
                               message)) {
        return false;
    }
    const DocumentSource* source = document.FindSource(mediaId);
    sourceIn = RationalTime(0, 1);
    sourceOut = source->duration;
    if (!ReadTime(args, "source_in", tool, false, sourceIn, message) ||
        !ReadTime(args, "source_out", tool, false, sourceOut, message)) {
        return Fail(errorName, message, message);
    }
    if (sourceIn.value < 0 || sourceOut.value < 0 || !(sourceIn < sourceOut) ||
        sourceOut > source->duration) {
        return Fail(errorName, message,
                    "'" + tool +
                        "' requires 0 <= source_in < source_out <= media "
                        "duration");
    }
    TempoBarSettings bars;
    if (!ReadTempoBarSettings(args, tool, bars, message)) {
        return Fail(errorName, message, message);
    }
    BeatDetectionResult beats;
    if (!backend.ReadSourceAudioBeats(mediaId, beats, message)) {
        return Fail(errorName, message, message);
    }
    grid = FitTempoGrid(beats.onsets, sourceIn, sourceOut, TempoGridSettings{},
                        bars);
    return true;
}

bool DispatchListTempoGrid(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id",  "source_in",     "source_out",
        "max_beats", "beats_per_bar", "bar_origin"};
    if (!CheckKnownKeys(args, kAllowed, "list_tempo_grid", message)) {
        return Fail(errorName, message, message);
    }
    Ulid mediaId;
    int32_t maxBeats = 64;
    if (!ReadId(args, "media_id", "list_tempo_grid", resolver, true, mediaId,
                message) ||
        !ReadInt32(args, "max_beats", "list_tempo_grid", false, maxBeats,
                   maxBeats, message)) {
        return Fail(errorName, message, message);
    }
    if (maxBeats < 1 || maxBeats > 512) {
        return Fail(errorName, message,
                    "'list_tempo_grid.max_beats' must be 1..512");
    }
    Document document;
    RationalTime sourceIn{0, 1};
    RationalTime sourceOut{0, 1};
    TempoGrid grid;
    if (!FitTempoGridForMedia(backend, "list_tempo_grid", args, mediaId,
                              document, sourceIn, sourceOut, grid, errorName,
                              message)) {
        return false;
    }

    Value response = Value::MakeObject();
    response.Set("media_id", Value::MakeString(mediaId));
    response.Set("source_in", BeatTimeValue(sourceIn));
    response.Set("source_out", BeatTimeValue(sourceOut));
    AppendTempoGridFields(response, grid);
    Value selected = Value::MakeArray();
    bool truncated = false;
    RationalTime nextSourceIn{0, 1};
    for (const TempoGridBeat& beat : grid.beats) {
        const RationalTime time(beat.sample, grid.sample_rate);
        if (selected.AsArray().size() >= static_cast<size_t>(maxBeats)) {
            if (!truncated) nextSourceIn = time;
            truncated = true;
            continue;
        }
        Value item = Value::MakeObject();
        item.Set("index", Value::MakeInt(beat.index));
        item.Set("source_time", BeatTimeValue(time));
        item.Set("matched", Value::MakeBool(beat.matched));
        item.Set("residual_samples", Value::MakeInt(beat.residual_samples));
        if (grid.beats_per_bar > 0 && grid.has_origin) {
            item.Set("bar", Value::MakeInt(beat.bar));
            item.Set("beat_in_bar", Value::MakeInt(beat.beat_in_bar));
        }
        selected.Push(std::move(item));
    }
    response.Set("total_beats",
                 Value::MakeInt(static_cast<int64_t>(grid.beats.size())));
    response.Set(
        "returned_beats",
        Value::MakeInt(static_cast<int64_t>(selected.AsArray().size())));
    response.Set("beats", std::move(selected));
    response.Set("truncated", Value::MakeBool(truncated));
    if (truncated) {
        response.Set("next_source_in", BeatTimeValue(nextSourceIn));
    }
    resultJson = response.Dump();
    return true;
}

// Where the track's energy moves, beat by beat, on the grid list_tempo_grid
// fits (MusicStructure.h). The band envelope comes with the onsets of
// analyze_audio_beats, so this reads a cache and decodes nothing.
bool DispatchListMusicStructure(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id",  "source_in",     "source_out",
        "max_beats", "beats_per_bar", "bar_origin"};
    if (!CheckKnownKeys(args, kAllowed, "list_music_structure", message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    int32_t maxBeats = 128;
    if (!ReadId(args, "media_id", "list_music_structure", resolver, true,
                mediaId, message) ||
        !ReadInt32(args, "max_beats", "list_music_structure", false, maxBeats,
                   maxBeats, message))
        return Fail(errorName, message, message);
    if (maxBeats < 1 || maxBeats > 512)
        return Fail(errorName, message,
                    "'list_music_structure.max_beats' must be 1..512");
    Document document;
    RationalTime sourceIn{0, 1};
    RationalTime sourceOut{0, 1};
    TempoGrid grid;
    if (!FitTempoGridForMedia(backend, "list_music_structure", args, mediaId,
                              document, sourceIn, sourceOut, grid, errorName,
                              message))
        return false;
    BeatDetectionResult analysis;
    if (!backend.ReadSourceAudioBeats(mediaId, analysis, message))
        return Fail(errorName, message, message);
    if (!analysis.bands)
        return Fail(errorName, message,
                    "the beat analysis of media '" + mediaId +
                        "' predates its band measurement: run "
                        "analyze_audio_beats again");
    std::vector<MusicBeatEnergy> beats;
    if (!MeasureBeatEnergies(*analysis.bands, grid, beats, message))
        return Fail(errorName, message, message);
    const std::vector<MusicEnergyChange> changes =
        FindEnergyChanges(beats, MusicEnergySettings{});

    const bool bars = grid.beats_per_bar > 0 && grid.has_origin;
    Value response = Value::MakeObject();
    response.Set("media_id", Value::MakeString(mediaId));
    response.Set("source_in", BeatTimeValue(sourceIn));
    response.Set("source_out", BeatTimeValue(sourceOut));
    AppendTempoGridFields(response, grid);
    response.Set("level_unit", Value::MakeString("centi_db_from_loudest"));
    Value changeList = Value::MakeArray();
    for (const MusicEnergyChange& change : changes) {
        Value item = Value::MakeObject();
        item.Set("kind", Value::MakeString(change.kind));
        item.Set("beat_index", Value::MakeInt(change.index));
        item.Set("source_time",
                 BeatTimeValue(RationalTime(change.sample, grid.sample_rate)));
        if (bars) {
            item.Set("bar", Value::MakeInt(change.bar));
            item.Set("beat_in_bar", Value::MakeInt(change.beat_in_bar));
        }
        item.Set("low_change", Value::MakeInt(change.low_change_centi_db));
        item.Set("full_change", Value::MakeInt(change.full_change_centi_db));
        changeList.Push(std::move(item));
    }
    response.Set("changes", std::move(changeList));
    Value columns = Value::MakeArray();
    for (const char* name : {"index", "source_sample", "low", "high", "full"})
        columns.Push(Value::MakeString(name));
    if (bars) {
        columns.Push(Value::MakeString("bar"));
        columns.Push(Value::MakeString("beat_in_bar"));
    }
    response.Set("beat_columns", std::move(columns));
    Value rows = Value::MakeArray();
    bool truncated = false;
    RationalTime nextSourceIn{0, 1};
    for (const MusicBeatEnergy& beat : beats) {
        if (rows.AsArray().size() >= static_cast<size_t>(maxBeats)) {
            if (!truncated)
                nextSourceIn = RationalTime(beat.sample, grid.sample_rate);
            truncated = true;
            continue;
        }
        Value row = Value::MakeArray();
        row.Push(Value::MakeInt(beat.index));
        row.Push(Value::MakeInt(beat.sample));
        row.Push(Value::MakeInt(beat.low_centi_db));
        row.Push(Value::MakeInt(beat.high_centi_db));
        row.Push(Value::MakeInt(beat.full_centi_db));
        if (bars) {
            row.Push(Value::MakeInt(beat.bar));
            row.Push(Value::MakeInt(beat.beat_in_bar));
        }
        rows.Push(std::move(row));
    }
    response.Set("beats", std::move(rows));
    response.Set("truncated", Value::MakeBool(truncated));
    if (truncated) response.Set("next_source_in", BeatTimeValue(nextSourceIn));
    resultJson = response.Dump();
    return true;
}

bool DispatchListClipTempoBeats(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "media_id",  "clip_id",       "source_in", "source_out",
        "max_beats", "beats_per_bar", "bar_origin"};
    if (!CheckKnownKeys(args, kAllowed, "list_clip_tempo_beats", message)) {
        return Fail(errorName, message, message);
    }
    Ulid mediaId;
    Ulid clipId;
    int32_t maxBeats = 64;
    if (!ReadId(args, "media_id", "list_clip_tempo_beats", resolver, true,
                mediaId, message) ||
        !ReadId(args, "clip_id", "list_clip_tempo_beats", resolver, false,
                clipId, message) ||
        !ReadInt32(args, "max_beats", "list_clip_tempo_beats", false, maxBeats,
                   maxBeats, message)) {
        return Fail(errorName, message, message);
    }
    if (maxBeats < 1 || maxBeats > 512) {
        return Fail(errorName, message,
                    "'list_clip_tempo_beats.max_beats' must be 1..512");
    }
    Document document;
    RationalTime sourceIn{0, 1};
    RationalTime sourceOut{0, 1};
    TempoGrid grid;
    if (!FitTempoGridForMedia(backend, "list_clip_tempo_beats", args, mediaId,
                              document, sourceIn, sourceOut, grid, errorName,
                              message)) {
        return false;
    }

    Value response = Value::MakeObject();
    response.Set("media_id", Value::MakeString(mediaId));
    response.Set("timeline_id", Value::MakeString(document.sequence.id));
    Value frameRate = Value::MakeObject();
    frameRate.Set("num", Value::MakeInt(document.sequence.frame_rate.num));
    frameRate.Set("den", Value::MakeInt(document.sequence.frame_rate.den));
    response.Set("frame_rate", std::move(frameRate));
    response.Set("source_in", BeatTimeValue(sourceIn));
    response.Set("source_out", BeatTimeValue(sourceOut));
    AppendTempoGridFields(response, grid);

    Value clips = Value::MakeArray();
    Value junctions = Value::MakeArray();
    int64_t placedClips = 0;
    if (grid.accepted()) {
        for (const DocumentTrack& track : document.sequence.tracks) {
            std::vector<DocumentClip> ordered;
            for (const DocumentClip& clip : track.clips) {
                if (clip.source_id != mediaId) continue;
                if (!clipId.empty() && clip.id != clipId) continue;
                ordered.push_back(clip);
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const DocumentClip& left, const DocumentClip& right) {
                          return left.timeline_in < right.timeline_in;
                      });
            for (const DocumentClip& clip : ordered) {
                TempoGridClipBeats placed;
                std::string projectionError;
                if (!ProjectTempoGridOntoClip(grid, clip,
                                              document.sequence.frame_rate,
                                              placed, projectionError)) {
                    return Fail(errorName, message, projectionError);
                }
                ++placedClips;
                Value entry = Value::MakeObject();
                entry.Set("clip_id", Value::MakeString(clip.id));
                entry.Set("track_id", Value::MakeString(track.id));
                entry.Set("track_index", Value::MakeInt(track.index));
                entry.Set("source_in", BeatTimeValue(clip.source_in));
                entry.Set("timeline_in", BeatTimeValue(clip.timeline_in));
                entry.Set("duration", BeatTimeValue(clip.duration));
                Value list = Value::MakeArray();
                bool truncated = false;
                for (const TempoGridSequenceBeat& beat : placed.beats) {
                    if (list.AsArray().size() >=
                        static_cast<size_t>(maxBeats)) {
                        truncated = true;
                        break;
                    }
                    Value item = Value::MakeObject();
                    item.Set("index", Value::MakeInt(beat.index));
                    if (grid.beats_per_bar > 0 && grid.has_origin) {
                        item.Set("bar", Value::MakeInt(beat.bar));
                        item.Set("beat_in_bar",
                                 Value::MakeInt(beat.beat_in_bar));
                    }
                    item.Set("source_time", BeatTimeValue(beat.source_time));
                    item.Set("timeline_time",
                             BeatTimeValue(beat.timeline_time));
                    item.Set("frame", Value::MakeInt(beat.frame));
                    item.Set("frame_offset", BeatTimeValue(beat.frame_offset));
                    item.Set("frame_offset_samples",
                             Value::MakeInt(beat.frame_offset_samples));
                    item.Set("frame_offset_milliseconds",
                             BeatNumberValue(
                                 TempoMilliseconds(beat.frame_offset_samples,
                                                   grid.sample_rate),
                                 9));
                    list.Push(std::move(item));
                }
                entry.Set(
                    "total_beats",
                    Value::MakeInt(static_cast<int64_t>(placed.beats.size())));
                entry.Set("returned_beats", Value::MakeInt(static_cast<int64_t>(
                                                list.AsArray().size())));
                entry.Set("beats", std::move(list));
                entry.Set("truncated", Value::MakeBool(truncated));
                clips.Push(std::move(entry));
            }
            for (const TempoGridJunction& junction :
                 TempoGridJunctions(grid, ordered)) {
                Value entry = Value::MakeObject();
                entry.Set("track_id", Value::MakeString(track.id));
                entry.Set("before_clip_id",
                          Value::MakeString(junction.before_clip_id));
                entry.Set("after_clip_id",
                          Value::MakeString(junction.after_clip_id));
                entry.Set("continuous", Value::MakeBool(junction.continuous));
                entry.Set("sample_offset",
                          Value::MakeInt(junction.sample_offset));
                entry.Set(
                    "offset_milliseconds",
                    BeatNumberValue(TempoMilliseconds(junction.sample_offset,
                                                      grid.sample_rate),
                                    9));
                entry.Set("beat_offset", Value::MakeInt(junction.beat_offset));
                entry.Set("bar_phase_offset",
                          Value::MakeInt(junction.bar_phase_offset));
                junctions.Push(std::move(entry));
            }
        }
    }
    response.Set("matching_clips", Value::MakeInt(placedClips));
    response.Set("clips", std::move(clips));
    response.Set("junctions", std::move(junctions));
    resultJson = response.Dump();
    return true;
}

bool DispatchAnalyzeTimelineAudio(McpBackend& backend, const IdResolver&,
                                  const Value& args, std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    const std::string tool = "analyze_timeline_audio";
    if (!CheckKnownKeys(args,
                        {"timeline_id", "dip_position_percent",
                         "minimum_dip_ms", "head_ms"},
                        tool, message))
        return Fail(errorName, message, message);
    TimelineAudioSettings settings;
    if (!ReadInt64(args, "dip_position_percent", tool, false,
                   settings.dip_position_percent, settings.dip_position_percent,
                   message) ||
        !ReadInt64(args, "minimum_dip_ms", tool, false,
                   settings.minimum_dip_milliseconds,
                   settings.minimum_dip_milliseconds, message) ||
        !ReadInt64(args, "head_ms", tool, false, settings.head_milliseconds,
                   settings.head_milliseconds, message))
        return Fail(errorName, message, message);
    if (settings.dip_position_percent < 0 ||
        settings.dip_position_percent > 100 ||
        settings.minimum_dip_milliseconds < 10 ||
        settings.head_milliseconds < 10 || settings.head_milliseconds > 60000)
        return Fail(errorName, message,
                    "'analyze_timeline_audio' settings are out of range");
    if (!backend.AnalyzeTimelineAudio(settings, resultJson, message)) {
        errorName = "IoError";
        return false;
    }
    return true;
}

}  // namespace

void RegisterSoundTools(const McpToolAdder& add) {
    add("measure_audio_offset",
        "How far one rush's sound sits from another's of the same take (a "
        "recorder's file against the camera's sound): `offset` is where "
        "other_id's first sample falls on reference_id, exact at 48 kHz, with "
        "the overlap, a confidence and the clocks' drift measured over head, "
        "middle and tail. Read only.",
        SchemaBuilder()
            .Field("reference_id",
                   IdSchema("The media heard as the reference, usually the "
                            "camera's rush."),
                   true)
            .Field("other_id",
                   IdSchema("The media placed against it, usually the "
                            "recorder's file."),
                   true)
            .Field("reference_channel",
                   EnumSchema({"mix", "left", "right"},
                              "Channel of reference_id to read (default "
                              "mix)."),
                   false)
            .Field("other_channel",
                   EnumSchema({"mix", "left", "right"},
                              "Channel of other_id to read: a recorder "
                              "may hold the lavalier on one and the boom "
                              "on the other (default mix)."),
                   false)
            .Build("measure_audio_offset arguments"),
        DispatchMeasureAudioOffset);

    add("propose_ducking",
        "Where a rush's voice speaks, and the gain curve a music under it "
        "would follow. The curve is 0 dB, down depth_db from attack_ms before "
        "each range to its start, held to its end, back up over release_ms; "
        "ranges whose ramps meet stay ducked between them. Every time is "
        "exact, in samples at 48 kHz, as {value, rate}, in the rush's own "
        "time. Read only (the clip gain cannot be animated yet).",
        SchemaBuilder()
            .Field("media_id", IdSchema("The rush whose voice to read."), true)
            .Field("channel",
                   EnumSchema({"mix", "left", "right"},
                              "Channel to read: a recorder may hold the "
                              "lavalier on one (default mix)."),
                   false)
            .Field("threshold_db",
                   IntSchema("Over the noise floor, in dB, for speech to "
                             "begin (1..60, default 10)."),
                   false)
            .Field("hysteresis_db",
                   IntSchema("Under the threshold, in dB, for speech to "
                             "end (0..threshold_db - 1, default 4)."),
                   false)
            .Field("floor_percentile",
                   IntSchema("Percentile of the frames' levels taken for "
                             "the noise floor (1..50, default 10)."),
                   false)
            .Field("minimum_speech_ms",
                   IntSchema("Shortest range kept (0..10000, default 200)."),
                   false)
            .Field("bridge_ms",
                   IntSchema("Gaps shorter than this are closed (0..10000, "
                             "default 300)."),
                   false)
            .Field("depth_db",
                   IntSchema("How far the music goes down, in dB (1..60, "
                             "default 12)."),
                   false)
            .Field("attack_ms",
                   IntSchema("How long before the voice the music starts "
                             "down (0..10000, default 150)."),
                   false)
            .Field("release_ms",
                   IntSchema("How long after the voice the music takes to "
                             "come back (0..10000, default 400)."),
                   false)
            .Build("propose_ducking arguments"),
        DispatchProposeDucking);

    add("sync_audio",
        "Lay a recorder's sound under a picture clip, linked: the offset is "
        "measured between the picture's rush and media_id as "
        "measure_audio_offset measures it (or taken from `offset`, its "
        "answer), the sound laid over the stretch of the picture the recorder "
        "holds, on the timeline's frames, its source_in on the sample, and "
        "linked to the picture's group with its exact phase. `placed` names "
        "the new clip. With sound_clip_id, a sound already on the timeline is "
        "slid into sync on its track and cut to the picture; `unchanged` says "
        "it already was.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("The picture clip."), true)
            .Field("media_id",
                   IdSchema("The recorder's media; with track_id, unless "
                            "sound_clip_id is given."),
                   false)
            .Field("track_id",
                   IdSchema("The audio track the sound goes on, with "
                            "media_id."),
                   false)
            .Field("sound_clip_id",
                   IdSchema("The recorder's sound already on an audio "
                            "track, instead of media_id and track_id."),
                   false)
            .Field("offset", kTimeSchemaText, false)
            .Field("picture_channel",
                   EnumSchema({"mix", "left", "right"},
                              "Channel of the picture's rush to measure on "
                              "(default mix)."),
                   false)
            .Field("recorder_channel",
                   EnumSchema({"mix", "left", "right"},
                              "Channel of the recorder to measure on "
                              "(default mix)."),
                   false)
            .Field("mute_camera_sound",
                   BoolSchema("Turn off the track of the picture's own "
                              "linked sound; default true."),
                   false)
            .Build("sync_audio arguments"),
        DispatchSyncAudio);

    add("render_sound",
        "Render a ditty (tools/cutmachine-sound: Dittytoy code, with edit, "
        "at() and sleepUntil()) on the cuts of this timeline as it is now, to "
        "a 24-bit WAV stem whose BWF timecode is the timeline's start "
        "(01:00:00:00 unless start_timecode). The answer is cm-sound's report: "
        "`out`, `sha256`, peaks, `loops` (with `triggers` and their `notes` "
        "when asked), `edit`. Then ingest_media the stem and place_sound_stem "
        "it. ditty and partition are read at the path given; a relative `out` "
        "is taken from the project's folder; without `out`, "
        "Sound/<name>-seed<n>.wav in the package.",
        SchemaBuilder()
            .Field("ditty",
                   StringSchema("The ditty, or with a partition the "
                                "instrument set (.js)."),
                   true)
            .Field("partition",
                   StringSchema("A partition (.json, cutmachine-partition)."),
                   false)
            .Field("seed", IntSchema("Random seed; 1 by default."), false)
            .Field("settings",
                   "{\"type\":\"object\",\"description\":\"Values for "
                   "the ditty's input.* sliders: numbers or texts.\"}",
                   false)
            .Field("sample_rate",
                   IntSchema("Samples per second; 48000 by default."), false)
            .Field("tail",
                   "{\"type\":\"number\",\"description\":\"Seconds "
                   "rendered past the edit's end (a reverb tail).\"}",
                   false)
            .Field("track_id",
                   IdSchema("The video track that makes the cuts, when "
                            "several hold clips."),
                   false)
            .Field("start_timecode",
                   StringSchema("Timecode of the timeline's first frame; "
                                "01:00:00:00 by default."),
                   false)
            .Field("out", StringSchema("Where the stem goes (.wav)."), false)
            .Field("stems", BoolSchema("Also one WAV per loop, beside `out`."),
                   false)
            .Field("triggers",
                   BoolSchema("Report each loop's trigger samples and their "
                              "notes."),
                   false)
            .Field("timeout",
                   IntSchema("Seconds before the render is stopped; 300 by "
                             "default."),
                   false)
            .Build("render_sound arguments"),
        DispatchRenderSound);

    add("place_sound_stem",
        "Lay a stem rendered by cutmachine-sound (tools/cutmachine-sound, "
        "render --project) where its BWF timecode says. Laid over the whole "
        "frames it covers (`placed.not_laid`: less than a frame of its end, a "
        "reverb tail), on a new audio track above every track, or on track_id.",
        SchemaBuilder()
            .Field("media_id", IdSchema("The stem, a WAV in the library."),
                   true)
            .Field("track_id",
                   IdSchema("An audio track to lay it on; omitted: a new "
                            "one."),
                   false)
            .Field("start_timecode",
                   StringSchema("Timecode of the timeline's first frame the "
                                "stem was rendered at; 01:00:00:00 by "
                                "default."),
                   false)
            .Build("place_sound_stem arguments"),
        DispatchPlaceSoundStem);

    add("analyze_audio_beats",
        "Decode one source's audio and cache spectral-flux onsets plus a tempo "
        "estimate. Then read it with list_audio_beats or fit a grid with "
        "list_tempo_grid. This analysis does not edit the project.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Audio-bearing media to analyse."),
                   true)
            .Build("analyze_audio_beats arguments"),
        DispatchAnalyzeAudioBeats);

    add("list_audio_beats",
        "Read cached onset positions for one source, optionally within an "
        "exact half-open source range. Results are bounded; when truncated, "
        "pass next_source_in as the next source_in. Strength is normalized "
        "within the source and tempo is estimated for the whole source. "
        "Run analyze_audio_beats first if no cache exists.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Analysed media to inspect."), true)
            .Field("source_in", kTimeSchemaText, false)
            .Field("source_out", kTimeSchemaText, false)
            .Field("max_onsets",
                   PositiveIntSchema("Maximum returned attacks (default "
                                     "128, maximum 512)."),
                   false)
            .Field("minimum_strength_percent",
                   IntSchema("Filter relative normalized strength 0..100 "
                             "(default 0)."),
                   false)
            .Build("list_audio_beats arguments"),
        DispatchListAudioBeats);

    // R7 -- docs/roadmap/REEL-2026-09.md. Reads beside list_audio_beats:
    // same cache, no new analysis, no operation.
    add("list_tempo_grid",
        "Fit an editorial beat grid onto the onsets analyze_audio_beats "
        "already cached for one source, and report it. The period and the "
        "phase are exact integer sample counts at the cache's rate, and every "
        "beat's residual against the nearest attack is published in samples. "
        "This is not analyze_audio_beats's bpm field: that one autocorrelates "
        "the whole source and reads a half-time tempo on ordinary music. "
        "Half-time and double-time readings are settled here by the attacks "
        "that fall between two candidate beats, and `doublings`/`halvings` say "
        "whether that happened. A track with no steady pulse answers "
        "accepted:false with its residuals rather than a grid forced onto it. "
        "Run analyze_audio_beats first if no cache exists. This read does not "
        "edit the project.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Analysed media to fit."), true)
            .Field("source_in", kTimeSchemaText, false)
            .Field("source_out", kTimeSchemaText, false)
            .Field("max_beats",
                   PositiveIntSchema("Maximum returned beats (default 64, "
                                     "maximum 512)."),
                   false)
            .Field("beats_per_bar",
                   IntSchema("Beats in one bar, 1..32. Declared by you; "
                             "without it no bar is numbered."),
                   false)
            .Field("bar_origin",
                   "{\"type\":\"object\",\"description\":\"Source time of a "
                   "beat you know opens a bar. Snapped to the nearest grid "
                   "beat; the distance is reported.\",\"properties\":{"
                   "\"value\":{\"type\":\"integer\"},"
                   "\"rate\":{\"type\":\"integer\"}},"
                   "\"required\":[\"value\",\"rate\"]}",
                   false)
            .Build("list_tempo_grid arguments"),
        DispatchListTempoGrid);

    add("list_music_structure",
        "Where a track opens up and where it breathes, on its beat grid: for "
        "every beat of the grid list_tempo_grid fits (same arguments), the "
        "energy of the low band (under 150 Hz: kick and bass), the high band "
        "(over 4 kHz) and the whole spectrum, in hundredths of a decibel under "
        "the loudest beat of the window, as compact rows. `changes` names the "
        "beats where the low band steps up (rise: a drop, the bass comes in) "
        "or down (fall: a break) by 6 dB or more between the two bars before "
        "and the bar after -- once per step, on the first beat at the new "
        "level. Read from the cache analyze_audio_beats writes; a cache made "
        "before this measurement existed asks for that analysis again. This "
        "read does not edit the project.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Analysed music to read."), true)
            .Field("source_in", kTimeSchemaText, false)
            .Field("source_out", kTimeSchemaText, false)
            .Field("max_beats",
                   PositiveIntSchema("Maximum returned beat rows (default "
                                     "128, maximum 512); changes are always "
                                     "all returned."),
                   false)
            .Field("beats_per_bar",
                   IntSchema("Beats in one bar, 1..32, as list_tempo_grid."),
                   false)
            .Field("bar_origin",
                   "{\"type\":\"object\",\"description\":\"Source time of a "
                   "beat that opens a bar, as list_tempo_grid.\","
                   "\"properties\":{\"value\":{\"type\":\"integer\"},"
                   "\"rate\":{\"type\":\"integer\"}},"
                   "\"required\":[\"value\",\"rate\"]}",
                   false)
            .Build("list_music_structure arguments"),
        DispatchListMusicStructure);

    add("list_clip_tempo_beats",
        "Place one source's beat grid on the timeline: for every clip that "
        "reads that media, each beat's sequence frame, and the exact gap "
        "between the beat and the start of that frame, in samples and in "
        "milliseconds. Use it to see which frame a cut on a beat lands on "
        "before placing it. Junctions between two adjacent clips of the same "
        "media report whether the grid runs through the cut; when it does not, "
        "the offset is named in samples, and beat_offset says how many beats "
        "of the track the cut skipped. Takes the same grid arguments as "
        "list_tempo_grid. This read does not edit the project and adds no "
        "operation: you place the cuts.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Analysed media to place."), true)
            .Field("clip_id",
                   IdSchema("Restrict to this clip; default every clip of "
                            "the media on the timeline."),
                   false)
            .Field("source_in", kTimeSchemaText, false)
            .Field("source_out", kTimeSchemaText, false)
            .Field("max_beats",
                   PositiveIntSchema("Maximum returned beats per clip "
                                     "(default 64, maximum 512)."),
                   false)
            .Field("beats_per_bar",
                   IntSchema("Beats in one bar, 1..32. Declared by you."),
                   false)
            .Field("bar_origin",
                   "{\"type\":\"object\",\"description\":\"Source time of a "
                   "beat you know opens a bar.\",\"properties\":{"
                   "\"value\":{\"type\":\"integer\"},"
                   "\"rate\":{\"type\":\"integer\"}},"
                   "\"required\":[\"value\",\"rate\"]}",
                   false)
            .Build("list_clip_tempo_beats arguments"),
        DispatchListClipTempoBeats);

    add("analyze_timeline_audio",
        "Measure the sound of a timeline as it plays, assembled exactly as "
        "transcribe_timeline and the export assemble it, without exporting. "
        "Reports integrated loudness, true peak and loudness range (EBU R128), "
        "the level and peak of the opening seconds with the air before the "
        "first voice, and every hollow: a stretch closer to the noise floor of "
        "the rush playing it than to its speech level (dip_position_percent of "
        "the way from floor to speech), or a gap where no audio plays. Floor "
        "and speech level come from analyze_speech_onset; a rush without one "
        "is listed in sources_without_floor and its windows are not judged.",
        SchemaBuilder()
            .Field("timeline_id", StringSchema("Full timeline ID to measure."),
                   true)
            .Field("dip_position_percent",
                   IntSchema("Threshold between the rush's floor (0) and its "
                             "speech level (100), in dB (default 50)."),
                   false)
            .Field("minimum_dip_ms",
                   IntSchema("Shortest hollow reported, in milliseconds "
                             "(default 250)."),
                   false)
            .Field("head_ms",
                   IntSchema("Length of the opening stretch measured on its "
                             "own (default 1500)."),
                   false)
            .Build("analyze_timeline_audio arguments"),
        DispatchAnalyzeTimelineAudio);
}
