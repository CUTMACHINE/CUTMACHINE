#include "SpeakerDiarization.h"

#include "FfmpegBinary.h"
#include "Json.h"
#include "LocalEnv.h"
#include "ProcessLauncher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#ifndef CUTMACHINE_DIARIZATION_PYTHON_DEFAULT
#define CUTMACHINE_DIARIZATION_PYTHON_DEFAULT ""
#endif
#ifndef CUTMACHINE_DIARIZATION_SCRIPT_DEFAULT
#define CUTMACHINE_DIARIZATION_SCRIPT_DEFAULT ""
#endif
#ifndef CUTMACHINE_DIARIZATION_MODEL_DEFAULT
#define CUTMACHINE_DIARIZATION_MODEL_DEFAULT ""
#endif

namespace {

using mcp_json::Value;

constexpr const char* kNotFoundPrefix = "diarization model not found: ";
constexpr int64_t kCacheVersion = 1;

const tool_files::Role kPython = {
    "the diarization's Python", kDiarizationPythonVariable,
    "diarization/env/bin/python", "build/diarization/env/bin/python"};
const tool_files::Role kScript = {
    "scripts/diarize.py", kDiarizationScriptVariable, "../scripts/diarize.py",
    "scripts/diarize.py"};
const tool_files::Role kModel = {
    "the speaker embedding model", kDiarizationModelVariable,
    "diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx",
    "build/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx"};

RationalTime SampleTime(int64_t samples) {
    return {samples, kDiarizationSampleRate};
}

// `time` in samples, rounded down or up. Negative times are not met here.
int64_t Samples(const RationalTime& time, bool up) {
    const __int128 numerator =
        static_cast<__int128>(time.value) * kDiarizationSampleRate;
    const __int128 whole = numerator / time.rate;
    return static_cast<int64_t>(whole +
                                ((up && numerator % time.rate != 0) ? 1 : 0));
}

bool ReadInt(const Value& value, int64_t& number) {
    return value.AsInt64(number);
}

std::string TextOf(const Value& object, const char* key) {
    const Value* field = object.Find(key);
    return field != nullptr && field->IsString() ? field->AsString()
                                                 : std::string();
}

// The segments and the similarities, which the report and the cache spell
// alike but for the segments' shape: objects in the report, triples in the
// cache.
bool ReadSegments(const Value& list, bool triples, int64_t samples,
                  int32_t speakers, std::vector<SpeakerSegment>& segments,
                  std::string& error) {
    if (!list.IsArray()) {
        error = "'segments' is not a list";
        return false;
    }
    int64_t previousEnd = 0;
    for (const Value& item : list.AsArray()) {
        int64_t start = 0;
        int64_t end = 0;
        int64_t speaker = 0;
        bool read = false;
        if (triples) {
            read = item.IsArray() && item.AsArray().size() == 3 &&
                   ReadInt(item.AsArray()[0], start) &&
                   ReadInt(item.AsArray()[1], end) &&
                   ReadInt(item.AsArray()[2], speaker);
        } else {
            const Value* from = item.Find("start");
            const Value* to = item.Find("end");
            const Value* who = item.Find("speaker");
            read = from != nullptr && to != nullptr && who != nullptr &&
                   ReadInt(*from, start) && ReadInt(*to, end) &&
                   ReadInt(*who, speaker);
        }
        if (!read) {
            error = "a segment is not a start, an end and a speaker";
            return false;
        }
        if (start < previousEnd || end <= start ||
            (samples > 0 && end > samples)) {
            error = "the segments are not in order inside the sound";
            return false;
        }
        if (speaker < 0 || speaker >= speakers) {
            error = "a segment names voice " + std::to_string(speaker) +
                    " of " + std::to_string(speakers);
            return false;
        }
        previousEnd = end;
        segments.push_back({SampleTime(start), SampleTime(end),
                            static_cast<int32_t>(speaker)});
    }
    return true;
}

bool ReadSimilarity(const Value* list, int32_t speakers,
                    std::vector<SpeakerSimilarity>& similarity,
                    std::string& error) {
    if (list == nullptr) return true;
    if (!list->IsArray()) {
        error = "'similarity' is not a list";
        return false;
    }
    for (const Value& item : list->AsArray()) {
        int64_t first = 0;
        int64_t second = 0;
        int64_t value = 0;
        if (!item.IsArray() || item.AsArray().size() != 3 ||
            !ReadInt(item.AsArray()[0], first) ||
            !ReadInt(item.AsArray()[1], second) ||
            !ReadInt(item.AsArray()[2], value) || first < 0 ||
            second <= first || second >= speakers || value < -1000 ||
            value > 1000) {
            error = "a similarity is not two voices and a value per mille";
            return false;
        }
        similarity.push_back({static_cast<int32_t>(first),
                              static_cast<int32_t>(second),
                              static_cast<int32_t>(value)});
    }
    return true;
}

bool ReadCommon(const Value& object, int64_t& samples,
                SpeakerDiarization& diarization, std::string& error) {
    int64_t rate = 0;
    int64_t speakers = 0;
    int64_t heard = 0;
    const Value* rateField = object.Find("sample_rate");
    const Value* samplesField = object.Find("samples");
    const Value* speakersField = object.Find("speakers");
    const Value* heardField = object.Find("speakers_heard");
    if (rateField == nullptr || !ReadInt(*rateField, rate) ||
        rate != kDiarizationSampleRate || samplesField == nullptr ||
        !ReadInt(*samplesField, samples) || samples <= 0 ||
        speakersField == nullptr || !ReadInt(*speakersField, speakers) ||
        speakers < 1 || speakers > kMaxDiarizedSpeakers ||
        heardField == nullptr || !ReadInt(*heardField, heard) || heard < 1 ||
        heard > speakers) {
        error = "it lacks a sample_rate of " +
                std::to_string(kDiarizationSampleRate) +
                ", a length, or a count of voices from 1 to " +
                std::to_string(kMaxDiarizedSpeakers);
        return false;
    }
    diarization.speakers = static_cast<int32_t>(speakers);
    diarization.speakers_heard = static_cast<int32_t>(heard);
    diarization.speech_from = TextOf(object, "speech_from");
    diarization.engine = TextOf(object, "engine");
    diarization.engine_version = TextOf(object, "engine_version");
    diarization.embedding = TextOf(object, "embedding");
    return true;
}

}  // namespace

DiarizationSettings DiarizationSettings::FromEnvironment() {
    return LocateDiarization(CurrentDiarizationSearch()).Settings();
}

std::string MissingDiarizationInstallation(
    const DiarizationSettings& settings) {
    if (!settings.refusal.empty()) return settings.refusal;
    return tool_files::CheckPaths(kNotFoundPrefix,
                                  {{settings.python, &kPython},
                                   {settings.script, &kScript},
                                   {settings.model, &kModel}},
                                  local_env::LocalEnvFilePath());
}

DiarizationSettings DiarizationLocation::Settings() const {
    DiarizationSettings settings;
    settings.python = python.path;
    settings.script = script.path;
    settings.model = model.path;
    settings.refusal = refusal;
    return settings;
}

DiarizationLocation LocateDiarization(const DiarizationSearch& search) {
    tool_files::Context context;
    context.executable_directory = search.executable_directory;
    context.working_directory = search.working_directory;
    context.is_file = search.is_file;
    DiarizationLocation location;
    std::vector<std::string> problems;
    std::vector<const tool_files::Role*> roles;
    const auto locate = [&](const tool_files::Role& role,
                            const std::string& configured,
                            const std::string& build) {
        std::string problem;
        tool_files::File file =
            tool_files::Locate(role, configured, build, context, problem);
        if (!problem.empty()) {
            problems.push_back(std::move(problem));
            roles.push_back(&role);
        }
        return file;
    };
    location.python =
        locate(kPython, search.python_variable, search.build_python);
    location.script =
        locate(kScript, search.script_variable, search.build_script);
    location.model = locate(kModel, search.model_variable, search.build_model);
    if (!problems.empty()) {
        location.refusal = tool_files::Refusal(kNotFoundPrefix, problems, roles,
                                               search.settings_file);
    }
    return location;
}

DiarizationSearch CurrentDiarizationSearch() {
    DiarizationSearch search;
    search.python_variable = local_env::Value(kDiarizationPythonVariable);
    search.script_variable = local_env::Value(kDiarizationScriptVariable);
    search.model_variable = local_env::Value(kDiarizationModelVariable);
    search.executable_directory = ffmpeg::ExecutableDirectory();
    search.build_python = CUTMACHINE_DIARIZATION_PYTHON_DEFAULT;
    search.build_script = CUTMACHINE_DIARIZATION_SCRIPT_DEFAULT;
    search.build_model = CUTMACHINE_DIARIZATION_MODEL_DEFAULT;
    std::error_code error;
    const std::filesystem::path workingDirectory =
        std::filesystem::current_path(error);
    if (!error) search.working_directory = workingDirectory.string();
    search.settings_file = local_env::LocalEnvFilePath();
    return search;
}

std::string DiarizationRefusalName(const std::string& message) {
    return message.find(kNotFoundPrefix) != std::string::npos
               ? kDiarizationNotFoundErrorName
               : std::string();
}

std::string SerializeSpeakerDiarization(const SpeakerDiarization& diarization) {
    Value json = Value::MakeObject();
    json.Set("version", Value::MakeInt(kCacheVersion));
    json.Set("media_id", Value::MakeString(diarization.media_id));
    json.Set("sample_rate", Value::MakeInt(kDiarizationSampleRate));
    int64_t samples = 1;
    Value segments = Value::MakeArray();
    for (const SpeakerSegment& segment : diarization.segments) {
        Value item = Value::MakeArray();
        const int64_t start = Samples(segment.start, false);
        const int64_t end = Samples(segment.end, true);
        item.Push(Value::MakeInt(start));
        item.Push(Value::MakeInt(end));
        item.Push(Value::MakeInt(segment.speaker));
        segments.Push(std::move(item));
        samples = std::max(samples, end);
    }
    json.Set("samples", Value::MakeInt(samples));
    json.Set("speakers", Value::MakeInt(diarization.speakers));
    json.Set("speakers_heard", Value::MakeInt(diarization.speakers_heard));
    json.Set("speech_from", Value::MakeString(diarization.speech_from));
    json.Set("engine", Value::MakeString(diarization.engine));
    json.Set("engine_version", Value::MakeString(diarization.engine_version));
    json.Set("embedding", Value::MakeString(diarization.embedding));
    json.Set("segments", std::move(segments));
    Value similarity = Value::MakeArray();
    for (const SpeakerSimilarity& pair : diarization.similarity) {
        Value item = Value::MakeArray();
        item.Push(Value::MakeInt(pair.first));
        item.Push(Value::MakeInt(pair.second));
        item.Push(Value::MakeInt(pair.per_mille));
        similarity.Push(std::move(item));
    }
    json.Set("similarity", std::move(similarity));
    return json.Dump() + "\n";
}

bool DeserializeSpeakerDiarization(const std::string& json,
                                   SpeakerDiarization& diarization,
                                   std::string& error) {
    diarization = SpeakerDiarization{};
    Value object;
    std::string parseError;
    if (!Value::Parse(json, object, parseError) || !object.IsObject()) {
        error = "the speakers cache is not a JSON object: " + parseError;
        return false;
    }
    int64_t version = 0;
    const Value* versionField = object.Find("version");
    if (versionField == nullptr || !ReadInt(*versionField, version) ||
        version != kCacheVersion) {
        error = "the speakers cache is of a version this build does not read";
        return false;
    }
    diarization.media_id = TextOf(object, "media_id");
    int64_t samples = 0;
    const Value* segments = object.Find("segments");
    if (diarization.media_id.empty() ||
        !ReadCommon(object, samples, diarization, error) ||
        segments == nullptr ||
        !ReadSegments(*segments, true, samples, diarization.speakers,
                      diarization.segments, error) ||
        !ReadSimilarity(object.Find("similarity"), diarization.speakers,
                        diarization.similarity, error)) {
        error = "the speakers cache cannot be read: " +
                (error.empty() ? std::string("it names no rush") : error);
        diarization = SpeakerDiarization{};
        return false;
    }
    return true;
}

bool LoadSpeakerDiarization(const std::string& path,
                            SpeakerDiarization& diarization,
                            std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "no speakers cache at '" + path + "'";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    return DeserializeSpeakerDiarization(text, diarization, error);
}

bool ReadDiarizationReport(const std::string& json, const std::string& mediaId,
                           SpeakerDiarization& diarization,
                           std::string& error) {
    diarization = SpeakerDiarization{};
    Value object;
    std::string parseError;
    if (!Value::Parse(json, object, parseError) || !object.IsObject()) {
        error = "the diarization report is not a JSON object: " + parseError;
        return false;
    }
    int64_t samples = 0;
    const Value* segments = object.Find("segments");
    if (!ReadCommon(object, samples, diarization, error) ||
        segments == nullptr ||
        !ReadSegments(*segments, false, samples, diarization.speakers,
                      diarization.segments, error) ||
        !ReadSimilarity(object.Find("similarity"), diarization.speakers,
                        diarization.similarity, error)) {
        error = "the diarization report cannot be read: " +
                (error.empty() ? std::string("it has no segments") : error);
        diarization = SpeakerDiarization{};
        return false;
    }
    if (diarization.segments.empty()) {
        error = "the diarization found no stretch of speech";
        diarization = SpeakerDiarization{};
        return false;
    }
    diarization.media_id = mediaId;
    return true;
}

std::vector<SpeechStretch> SpeechStretchesOfWords(
    const std::vector<TranscriptWord>& words, const RationalTime& gap) {
    std::vector<SpeechStretch> stretches;
    for (const TranscriptWord& word : words) {
        if (word.start.rate <= 0 || word.end.rate <= 0 ||
            !(word.start < word.end) || word.start.value < 0)
            continue;
        if (!stretches.empty() && word.start.sub(stretches.back().end) < gap) {
            if (stretches.back().end < word.end)
                stretches.back().end = word.end;
            continue;
        }
        stretches.push_back({word.start, word.end});
    }
    return stretches;
}

std::string SpeechStretchesJson(const std::vector<SpeechStretch>& stretches) {
    Value list = Value::MakeArray();
    for (const SpeechStretch& stretch : stretches) {
        Value item = Value::MakeArray();
        item.Push(Value::MakeInt(Samples(stretch.start, false)));
        item.Push(Value::MakeInt(Samples(stretch.end, true)));
        list.Push(std::move(item));
    }
    return list.Dump() + "\n";
}

int32_t SpeakerOfSpan(const SpeakerDiarization& diarization,
                      const RationalTime& start, const RationalTime& end) {
    if (diarization.speakers <= 0 || !(start < end)) return -1;
    std::vector<RationalTime> overlap(static_cast<size_t>(diarization.speakers),
                                      RationalTime{0, 1});
    for (const SpeakerSegment& segment : diarization.segments) {
        if (!(segment.start < end)) break;
        if (!(start < segment.end)) continue;
        const RationalTime from = segment.start < start ? start : segment.start;
        const RationalTime to = end < segment.end ? end : segment.end;
        if (segment.speaker < 0 || segment.speaker >= diarization.speakers)
            continue;
        RationalTime& total = overlap[static_cast<size_t>(segment.speaker)];
        total = total.add(to.sub(from));
    }
    int32_t best = -1;
    for (int32_t speaker = 0; speaker < diarization.speakers; ++speaker) {
        const RationalTime& total = overlap[static_cast<size_t>(speaker)];
        if (total.value <= 0) continue;
        if (best < 0 || overlap[static_cast<size_t>(best)] < total)
            best = speaker;
    }
    return best;
}

std::vector<RationalTime> SpeakerDurations(
    const SpeakerDiarization& diarization) {
    std::vector<RationalTime> durations(
        static_cast<size_t>(std::max(0, diarization.speakers)),
        RationalTime{0, kDiarizationSampleRate});
    for (const SpeakerSegment& segment : diarization.segments) {
        if (segment.speaker < 0 || segment.speaker >= diarization.speakers)
            continue;
        RationalTime& total = durations[static_cast<size_t>(segment.speaker)];
        total = total.add(segment.end.sub(segment.start));
    }
    return durations;
}

std::vector<std::string> DiarizationArguments(
    const DiarizationSettings& settings, const DiarizationRequest& request) {
    std::vector<std::string> arguments = {
        settings.python, settings.script,
        "--audio",       request.audio_path,
        "--embedding",   settings.model,
        "--speakers",    std::to_string(request.speakers),
        "--report",      request.report_path};
    if (!request.speech_path.empty()) {
        arguments.insert(arguments.end(), {"--speech", request.speech_path});
    }
    return arguments;
}

bool RunDiarization(const DiarizationSettings& settings,
                    const DiarizationRequest& request,
                    MediaTaskContext& context, std::string& error) {
    if (request.speakers < 1 || request.speakers > kMaxDiarizedSpeakers) {
        error = "the count of voices must lie in [1, " +
                std::to_string(kMaxDiarizedSpeakers) + "]";
        return false;
    }
    if (request.audio_path.empty() || request.report_path.empty()) {
        error = "the diarization needs a sound and a report path";
        return false;
    }
    std::error_code removeError;
    std::filesystem::remove(request.report_path, removeError);

    process::Request launch;
    launch.arguments = DiarizationArguments(settings, request);
    launch.discard_input = true;
    launch.standard_output = process::Stream::kDiscard;
    launch.standard_error = process::Stream::kPipe;
    launch.error_tail_bytes = 4096;
    launch.cancelled = [&context] { return context.Cancelled(); };
    launch.on_started = [&context] { context.SetProgress(0.0, "Locuteurs"); };
    const process::Result outcome = process::Run(launch);
    if (!outcome.Started()) {
        error = "unable to start '" + settings.python +
                "': " + outcome.SystemErrorText();
        return false;
    }
    if (outcome.cancelled || context.Cancelled()) {
        error = "diarization cancelled";
        return false;
    }
    if (!outcome.Succeeded()) {
        // The script's progress lines are not its failure.
        std::string tail;
        size_t at = 0;
        while (at < outcome.error_tail.size()) {
            size_t line = outcome.error_tail.find('\n', at);
            if (line == std::string::npos) line = outcome.error_tail.size();
            const std::string text = outcome.error_tail.substr(at, line - at);
            if (text.rfind("progress ", 0) != 0 && !text.empty())
                tail += (tail.empty() ? "" : "\n") + text;
            at = line + 1;
        }
        error = tail.empty() ? "the diarization script failed" : tail;
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t bytes =
        std::filesystem::file_size(request.report_path, sizeError);
    if (sizeError || bytes == 0) {
        error = "the diarization script wrote no report at '" +
                request.report_path + "'";
        return false;
    }
    return true;
}
