#include "BeatDetection.h"
#include "Cli.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Json.h"
#include "McpLiveBackend.h"
#include "McpTools.h"
#include "MediaTaskManager.h"
#include "MotionCli.h"
#include "MusicStructure.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "TempoGrid.h"
#include "Ulid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

// Generates a synthetic click track: short sine bursts at every multiple of
// `periodSeconds`, starting at t=0, for `clickCount` clicks. This is the
// ground truth a real click track (e.g. a metronome or drum loop count-in)
// approximates, used to check onset detection against a known answer rather
// than eyeballing a real recording.
bool GenerateClickTrack(const std::filesystem::path& destination,
                        double periodSeconds, int clickCount,
                        uint32_t sampleRate) {
    constexpr double kPulseSeconds = 0.02;
    const double duration = periodSeconds * clickCount;
    std::ostringstream command;
    command << Quote(FFMPEG_EXECUTABLE)
            << " -hide_banner -loglevel error -f lavfi -i "
               "\"aevalsrc=exprs='if(lt(mod(t\\,"
            << periodSeconds << ")\\," << kPulseSeconds
            << ")\\,0.9*sin(2*PI*1000*t)\\,0)':s=" << sampleRate
            << ":d=" << duration << "\" -c:a pcm_s16le -y "
            << Quote(destination);
    return std::system(command.str().c_str()) == 0;
}

// Builds a click track at `bpm`, runs it through the real DetectAudioBeats /
// LoadAudioBeats pair via a MediaTaskManager (mirroring how the app enqueues
// this work), and checks detected onsets against the known click grid and
// the estimated BPM against the known tempo. Returns true when every check
// passes.
bool RunClickTrackCase(double bpm, int clickCount) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-beats");
    const std::filesystem::path source = root / "click.wav";
    const std::filesystem::path cache = root / "cache" / "click.beats";
    std::filesystem::create_directories(root);

    const double periodSeconds = 60.0 / bpm;
    constexpr uint32_t kSampleRate = 48000;

    if (!GenerateClickTrack(source, periodSeconds, clickCount, kSampleRate)) {
        std::cerr << "FAIL: unable to generate " << bpm
                  << " BPM click-track fixture\n";
        std::filesystem::remove_all(root);
        return false;
    }

    MediaTaskManager manager;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    const RationalTime duration{
        static_cast<int64_t>(std::llround(periodSeconds * clickCount * 1000)),
        1000};
    manager.Enqueue(MediaTaskKind::Beat, "beat detection fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        return DetectAudioBeats(source.string(), cache.string(),
                                                duration, settings, context,
                                                error);
                    });
    if (!manager.WaitForIdle(30000)) {
        std::cerr << "FAIL: beat detection task timed out\n";
        std::filesystem::remove_all(root);
        return false;
    }
    const auto tasks = manager.Snapshot();
    if (tasks.size() != 1 || tasks[0].state != MediaTaskState::Succeeded) {
        std::cerr << "FAIL: beat detection task did not succeed: "
                  << (tasks.empty() ? "" : tasks[0].error) << '\n';
        std::filesystem::remove_all(root);
        return false;
    }

    BeatDetectionResult result;
    std::string error;
    if (!LoadAudioBeats(cache.string(), result, error)) {
        std::cerr << "FAIL: unable to load beat detection cache: " << error
                  << '\n';
        std::filesystem::remove_all(root);
        return false;
    }

    // The very first click sits at t=0, with no preceding audio to rise
    // from — spectral flux cannot see an "increase" at the very first STFT
    // frame. Every click after the first sits after real silence and is
    // expected to be found. Ground truth therefore starts at index 1.
    std::vector<double> expectedOnsetSeconds;
    for (int i = 1; i < clickCount; ++i)
        expectedOnsetSeconds.push_back(i * periodSeconds);

    constexpr double kToleranceSeconds = 0.03;
    int matched = 0;
    for (double expected : expectedOnsetSeconds) {
        for (const AudioOnset& onset : result.onsets) {
            const double seconds =
                static_cast<double>(onset.time.value) / onset.time.rate;
            if (std::abs(seconds - expected) <= kToleranceSeconds) {
                ++matched;
                break;
            }
        }
    }
    const double matchRatio =
        expectedOnsetSeconds.empty()
            ? 0.0
            : static_cast<double>(matched) / expectedOnsetSeconds.size();

    // No detected onset should be far from every expected click either —
    // guards against spurious detections flooding the result.
    int spurious = 0;
    for (const AudioOnset& onset : result.onsets) {
        const double seconds =
            static_cast<double>(onset.time.value) / onset.time.rate;
        bool nearAny = seconds <= periodSeconds + kToleranceSeconds;
        for (double expected : expectedOnsetSeconds)
            if (std::abs(seconds - expected) <= kToleranceSeconds)
                nearAny = true;
        if (!nearAny) ++spurious;
    }

    bool ok = matchRatio >= 0.9 && spurious == 0;
    if (!ok) {
        std::cerr << "FAIL(" << bpm << " BPM): matched " << matched << "/"
                  << expectedOnsetSeconds.size() << " onsets, " << spurious
                  << " spurious, total detected " << result.onsets.size()
                  << '\n';
    } else {
        std::cout << "PASS: " << bpm << " BPM click-track onsets matched ("
                  << matched << "/" << expectedOnsetSeconds.size() << ")\n";
    }

    if (!result.bpm.has_value()) {
        std::cerr << "FAIL(" << bpm << " BPM): no BPM estimate produced\n";
        ok = false;
    } else {
        // Autocorrelation tempo induction is octave-ambiguous by nature: a
        // steady half- or double-tempo read is a known, accepted limitation
        // of this family of method, not a bug — so accept bpm, 2*bpm or
        // bpm/2 within tolerance.
        const bool matchesBpm = std::abs(*result.bpm - bpm) <= 2.0;
        const bool matchesDouble = std::abs(*result.bpm - 2 * bpm) <= 4.0;
        const bool matchesHalf = std::abs(*result.bpm - bpm / 2) <= 1.0;
        if (!(matchesBpm || matchesDouble || matchesHalf)) {
            std::cerr << "FAIL(" << bpm << " BPM): estimate " << *result.bpm
                      << " far from expected\n";
            ok = false;
        } else {
            std::cout << "PASS: " << bpm << " BPM estimate " << *result.bpm
                      << " (confidence " << result.bpm_confidence << ")\n";
        }
    }

    std::filesystem::remove_all(root);
    return ok;
}

// Settings that are internally inconsistent (a non-power-of-two FFT size,
// here) must be refused with an explicit error, never silently coerced.
bool RunInvalidSettingsAreRejected() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-beats-invalid");
    const std::filesystem::path source = root / "click.wav";
    const std::filesystem::path cache = root / "cache" / "click.beats";
    std::filesystem::create_directories(root);
    if (!GenerateClickTrack(source, 0.5, 4, 48000)) {
        std::cerr << "FAIL: unable to generate invalid-settings fixture\n";
        std::filesystem::remove_all(root);
        return false;
    }

    MediaTaskManager manager;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.fft_size = 1000;  // Not a power of two.
    manager.Enqueue(MediaTaskKind::Beat, "invalid settings",
                    [&](MediaTaskContext& context, std::string& error) {
                        return DetectAudioBeats(source.string(), cache.string(),
                                                {2, 1}, settings, context,
                                                error);
                    });
    manager.WaitForIdle(30000);
    const auto tasks = manager.Snapshot();
    const bool ok = tasks.size() == 1 &&
                    tasks[0].state == MediaTaskState::Failed &&
                    !tasks[0].error.empty();
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: non-power-of-two fft_size was not refused\n";
    } else {
        std::cout << "PASS: non-power-of-two fft_size refused explicitly\n";
    }
    return ok;
}

// F1.6 -- a cache copied from another analysis rate must not be exposed as
// a negative RationalTime rate after uint32->int32 conversion.
bool RunCorruptCacheIsRejected() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-beat-bad");
    const std::filesystem::path audio = root / "click.wav";
    const std::filesystem::path cache = root / "click.beats";
    std::filesystem::create_directories(root);
    if (!GenerateClickTrack(audio, 0.5, 6, 48000)) {
        std::filesystem::remove_all(root);
        return false;
    }
    MediaTaskManager tasks;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    tasks.Enqueue(MediaTaskKind::Beat, "cache validation",
                  [&](MediaTaskContext& context, std::string& error) {
                      return DetectAudioBeats(audio.string(), cache.string(),
                                              {3, 1}, settings, context, error);
                  });
    bool ok = tasks.WaitForIdle(30000);
    BeatDetectionResult result;
    std::string error;
    ok = ok && LoadAudioBeats(cache.string(), result, error);
    const auto before = result.onsets.size();
    std::fstream file(cache, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(8);  // Magic header precedes the uint32 analysis rate.
    const uint32_t impossibleRate = std::numeric_limits<uint32_t>::max();
    file.write(reinterpret_cast<const char*>(&impossibleRate),
               sizeof(impossibleRate));
    file.close();
    ok = ok && !LoadAudioBeats(cache.string(), result, error) &&
         result.onsets.size() == before &&
         error == "invalid beat detection cache header";
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: corrupt cache analysis rate was not refused\n";
    }
    return ok;
}

// F1.6 -- exercise the real CLI/MCP dispatcher against a stored audio-only
// rush. Analysis may write a cache, but the canonical project must remain
// byte-identical; a truncated result must page by exact source time.
bool RunProjectToolCase() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-beat-tools");
    const std::filesystem::path audio = root / "click.wav";
    std::filesystem::create_directories(root);
    bool ok = GenerateClickTrack(audio, 0.5, 16, 48000);
    std::string path;
    std::string error;
    std::string output;
    if (ok) {
        ok = CreatePortableProject(
            (root / "project").string(),
            Project::FromDocument(Document{}, "Beat tools"), path, error);
    }
    if (ok) {
        ok = IngestCommand(path, audio.string(), false, output) == 0;
    }
    if (!ok) {
        std::cerr << "FAIL: unable to prepare audio-only project: " << error
                  << ' ' << output << '\n';
        std::filesystem::remove_all(root);
        return false;
    }
    Project project;
    if (!LoadStoredProject(path, project, error) ||
        project.rushes.size() != 1 || !project.rushes[0].has_audio ||
        project.rushes[0].has_video) {
        std::cerr << "FAIL: ingest did not preserve an audio-only rush\n";
        std::filesystem::remove_all(root);
        return false;
    }
    const Ulid mediaId = project.rushes.front().id;
    std::ifstream input(path, std::ios::binary);
    const std::string before{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const std::string mediaArg = "{\"media_id\":\"" + mediaId + "\"}";
    if (CallToolCommand(path, "list_audio_beats", mediaArg, output) == 0) {
        std::cerr << "FAIL: missing beat cache was reported as available\n";
        ok = false;
    }
    if (CallToolCommand(path, "analyze_audio_beats", mediaArg, output) != 0) {
        std::cerr << "FAIL: CLI beat analysis: " << output << '\n';
        ok = false;
    }
    const std::string firstArg =
        "{\"media_id\":\"" + mediaId + "\",\"max_onsets\":3}";
    mcp_json::Value first;
    if (CallToolCommand(path, "list_audio_beats", firstArg, output) != 0 ||
        !mcp_json::Value::Parse(output, first, error)) {
        std::cerr << "FAIL: CLI beat listing: " << output << ' ' << error
                  << '\n';
        ok = false;
    } else {
        const auto* result = first.Find("result");
        const auto* beats =
            result == nullptr ? nullptr : result->Find("onsets");
        const auto* cursor =
            result == nullptr ? nullptr : result->Find("next_source_in");
        const auto* tempo = result == nullptr ? nullptr : result->Find("bpm");
        if (beats == nullptr || !beats->IsArray() ||
            beats->AsArray().size() != 3 || cursor == nullptr ||
            !cursor->IsObject() || tempo == nullptr || !tempo->IsNumber()) {
            std::cerr << "FAIL: bounded beat result/tempo/cursor: " << output
                      << '\n';
            ok = false;
        } else {
            const std::string secondArg =
                "{\"media_id\":\"" + mediaId +
                "\",\"max_onsets\":3,\"source_in\":" + cursor->Dump() + "}";
            mcp_json::Value second;
            if (CallToolCommand(path, "list_audio_beats", secondArg, output) !=
                    0 ||
                !mcp_json::Value::Parse(output, second, error)) {
                std::cerr << "FAIL: exact-time beat pagination: " << output
                          << '\n';
                ok = false;
            } else {
                const auto* secondResult = second.Find("result");
                const auto* secondBeats = secondResult == nullptr
                                              ? nullptr
                                              : secondResult->Find("onsets");
                const auto* firstNext =
                    secondBeats == nullptr || !secondBeats->IsArray() ||
                            secondBeats->AsArray().empty()
                        ? nullptr
                        : secondBeats->AsArray().front().Find("source_time");
                if (firstNext == nullptr ||
                    firstNext->Dump() != cursor->Dump()) {
                    std::cerr << "FAIL: second page did not begin at exact "
                                 "next onset\n";
                    ok = false;
                }
            }
        }
    }
    if (CallToolCommand(path, "list_audio_beats",
                        "{\"media_id\":\"" + mediaId + "\",\"max_onsets\":513}",
                        output) == 0 ||
        CallToolCommand(path, "list_audio_beats",
                        "{\"media_id\":\"" + mediaId +
                            "\",\"source_in\":{\"value\":9,\"rate\":1}}",
                        output) == 0) {
        std::cerr << "FAIL: beat listing accepted invalid bounds\n";
        ok = false;
    }
    std::ifstream afterInput(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(afterInput),
                            std::istreambuf_iterator<char>()};
    if (before != after) {
        std::cerr << "FAIL: beat analysis changed canonical project bytes\n";
        ok = false;
    }

    Document liveDocument = project.MakeActiveDocument();
    EditLog liveLog;
    McpLiveBackend live(liveDocument, liveLog);
    live.SetProjectPath(path);
    McpToolRegistry registry;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("media_id", mcp_json::Value::MakeString(mediaId));
    if (!registry.Call(live, "list_audio_beats", arguments).ok ||
        !registry.Call(live, "analyze_audio_beats", arguments).ok) {
        std::cerr << "FAIL: live backend did not share beat tools\n";
        ok = false;
    }
    std::filesystem::remove_all(root);
    if (ok) {
        std::cout << "PASS: CLI/MCP beat tools share exact cached audio\n";
    }
    return ok;
}

// R7 -- docs/roadmap/REEL-2026-09.md. The tempo grid reaches the agent
// surfaces through the same dispatcher list_audio_beats does, on the same
// cache: this exercises the CLI path end to end on a click track whose
// tempo is known, and checks that reading a grid leaves the project's bytes
// alone.
bool RunTempoGridToolCase() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-tempo-grid");
    const std::filesystem::path audio = root / "click.wav";
    std::filesystem::create_directories(root);
    // 64 clicks at 0.5 s: 32 s at exactly 120 BPM.
    bool ok = GenerateClickTrack(audio, 0.5, 64, 48000);
    std::string path;
    std::string error;
    std::string output;
    if (ok) {
        ok = CreatePortableProject(
            (root / "project").string(),
            Project::FromDocument(Document{}, "Tempo grid"), path, error);
    }
    if (ok) {
        ok = IngestCommand(path, audio.string(), false, output) == 0;
    }
    Project project;
    if (!ok || !LoadStoredProject(path, project, error) ||
        project.rushes.size() != 1) {
        std::cerr << "FAIL: unable to prepare a tempo grid project: " << error
                  << ' ' << output << '\n';
        std::filesystem::remove_all(root);
        return false;
    }
    const Ulid mediaId = project.rushes.front().id;
    const std::string mediaArg = "{\"media_id\":\"" + mediaId + "\"}";
    std::ifstream input(path, std::ios::binary);
    const std::string before{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};

    if (CallToolCommand(path, "analyze_audio_beats", mediaArg, output) != 0) {
        std::cerr << "FAIL: CLI beat analysis before the grid: " << output
                  << '\n';
        ok = false;
    }
    const std::string gridArg = "{\"media_id\":\"" + mediaId +
                                "\",\"beats_per_bar\":4,\"max_beats\":8}";
    mcp_json::Value grid;
    if (!ok || CallToolCommand(path, "list_tempo_grid", gridArg, output) != 0 ||
        !mcp_json::Value::Parse(output, grid, error)) {
        std::cerr << "FAIL: CLI tempo grid: " << output << ' ' << error << '\n';
        ok = false;
    } else {
        const mcp_json::Value* result = grid.Find("result");
        const mcp_json::Value* accepted =
            result == nullptr ? nullptr : result->Find("accepted");
        const mcp_json::Value* period =
            result == nullptr ? nullptr : result->Find("period_samples");
        const mcp_json::Value* beats =
            result == nullptr ? nullptr : result->Find("beats");
        const mcp_json::Value* rate =
            result == nullptr ? nullptr : result->Find("sample_rate");
        int64_t periodSamples = 0;
        int64_t sampleRate = 0;
        if (accepted == nullptr || !accepted->AsBool() || period == nullptr ||
            !period->AsInt64(periodSamples) || rate == nullptr ||
            !rate->AsInt64(sampleRate) || sampleRate <= 0 || beats == nullptr ||
            !beats->IsArray() || beats->AsArray().size() != 8) {
            std::cerr << "FAIL: a 120 BPM click track did not produce a "
                         "bounded grid: "
                      << output << '\n';
            ok = false;
        } else if (std::llabs(periodSamples * 2 - sampleRate) >
                   sampleRate / 200) {
            // Half a second per beat, in the cache's own decode rate, to
            // 0.5 %. The cache quantizes its onsets to the analysis hop
            // (11.6 ms at 44.1 kHz); the fit has to see through that.
            std::cerr << "FAIL: 120 BPM at " << sampleRate << " Hz fitted as "
                      << periodSamples << " samples per beat\n";
            ok = false;
        }
        // No bar was declared (beats_per_bar without bar_origin), so no beat
        // carries one: the engine does not guess the downbeat.
        if (ok && beats->AsArray().front().Find("bar") != nullptr) {
            std::cerr << "FAIL: a bar was numbered without a declared "
                         "origin\n";
            ok = false;
        }
    }
    // The projection tool answers on the same media even with no clip using
    // it, and it is a read like the others.
    mcp_json::Value placed;
    if (ok && (CallToolCommand(path, "list_clip_tempo_beats", mediaArg,
                               output) != 0 ||
               !mcp_json::Value::Parse(output, placed, error) ||
               placed.Find("result") == nullptr ||
               placed.Find("result")->Find("clips") == nullptr)) {
        std::cerr << "FAIL: CLI clip tempo beats: " << output << ' ' << error
                  << '\n';
        ok = false;
    }
    if (CallToolCommand(path, "list_tempo_grid",
                        "{\"media_id\":\"" + mediaId + "\",\"max_beats\":513}",
                        output) == 0 ||
        CallToolCommand(
            path, "list_tempo_grid",
            "{\"media_id\":\"" + mediaId + "\",\"beats_per_bar\":33}",
            output) == 0) {
        std::cerr << "FAIL: the tempo grid accepted invalid bounds\n";
        ok = false;
    }
    std::ifstream afterInput(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(afterInput),
                            std::istreambuf_iterator<char>()};
    if (before != after) {
        std::cerr << "FAIL: reading a tempo grid changed the project bytes\n";
        ok = false;
    }
    std::filesystem::remove_all(root);
    if (ok) {
        std::cout << "PASS: CLI tempo grid reads the cached onsets\n";
    }
    return ok;
}

}  // namespace

// The band envelope rides on the beat analysis: a click track at 120 BPM
// whose bass (60 Hz) comes in at 8 s. The real analysis, the grid fitted on
// its onsets and the structure read on the grid must name one rise, on the
// beat at 8 s; a cache that ends before the envelope still loads.
bool RunBandEnvelopeCase() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-bands");
    const std::filesystem::path audio = root / "drop.wav";
    const std::filesystem::path cache = root / "drop.beats";
    std::filesystem::create_directories(root);
    constexpr uint32_t kRate = 44100;
    std::ostringstream command;
    command << Quote(FFMPEG_EXECUTABLE)
            << " -hide_banner -loglevel error -f lavfi -i "
               "\"aevalsrc=exprs='if(lt(mod(t\\,0.5)\\,0.02)\\,"
               "0.5*sin(2*PI*1000*t)\\,0)+if(gte(t\\,8)\\,"
               "0.4*sin(2*PI*60*t)\\,0)':s="
            << kRate << ":d=16\" -c:a pcm_s16le -y " << Quote(audio);
    bool ok = std::system(command.str().c_str()) == 0;
    MediaTaskManager tasks;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    tasks.Enqueue(MediaTaskKind::Beat, "bands",
                  [&](MediaTaskContext& context, std::string& error) {
                      return DetectAudioBeats(audio.string(), cache.string(),
                                              {16, 1}, settings, context,
                                              error);
                  });
    ok = ok && tasks.WaitForIdle(30000);
    BeatDetectionResult result;
    std::string error;
    ok = ok && LoadAudioBeats(cache.string(), result, error);
    const bool enveloped =
        ok && result.bands && result.bands->window_size == 1024 &&
        result.bands->hop_size == 512 &&
        result.bands->full_centi_db.size() > (16 * kRate) / 512 - 4 &&
        result.bands->low_centi_db.size() == result.bands->full_centi_db.size();
    if (!enveloped)
        std::cerr << "FAIL: the analysis caches no band envelope: " << error
                  << '\n';
    bool rise = false;
    std::string found;
    if (enveloped) {
        const TempoGrid grid =
            FitTempoGrid(result.onsets, RationalTime(0, 1), RationalTime(16, 1),
                         TempoGridSettings{}, TempoBarSettings{});
        std::vector<MusicBeatEnergy> beats;
        if (grid.accepted() &&
            MeasureBeatEnergies(*result.bands, grid, beats, error)) {
            const std::vector<MusicEnergyChange> changes =
                FindEnergyChanges(beats, MusicEnergySettings{});
            for (const MusicEnergyChange& change : changes)
                found += ' ' + change.kind + '@' +
                         std::to_string(change.sample) + '(' +
                         std::to_string(change.low_change_centi_db) + ')';
            // The beat at 8 s, to within the grid's own tolerance.
            rise = changes.size() == 1 && changes[0].kind == "rise" &&
                   std::llabs(changes[0].sample - 8 * int64_t{kRate}) <
                       kRate / 20 &&
                   changes[0].low_change_centi_db >= 600;
        } else {
            found = " grid accepted " + std::to_string(grid.accepted()) + ": " +
                    error;
        }
        if (!rise)
            std::cerr << "FAIL: the bass coming in at 8 s is one rise, got"
                      << found << '\n';
    }
    // A cache from before the envelope: cut where the envelope starts.
    bool legacy = false;
    if (enveloped) {
        const auto size = std::filesystem::file_size(cache);
        const auto envelopeBytes =
            8 + 3 * sizeof(uint32_t) + sizeof(uint64_t) +
            3 * result.bands->full_centi_db.size() * sizeof(int32_t);
        std::filesystem::resize_file(cache, size - envelopeBytes);
        BeatDetectionResult old;
        legacy = LoadAudioBeats(cache.string(), old, error) && !old.bands &&
                 old.onsets.size() == result.onsets.size();
        if (!legacy)
            std::cerr << "FAIL: a cache without the envelope no longer loads: "
                      << error << '\n';
    }
    std::filesystem::remove_all(root);
    return enveloped && rise && legacy;
}

// A title placed on a beat: place_motion_clip's `beat` anchor names beat 10
// of a 120 BPM click track, and the engine answers the frame it is heard on
// -- 5 s, frame 125 at 25 i/s -- without the caller turning a beat into a
// time. Refused by name when the beat is not heard, and when it is heard
// twice.
bool RunBeatAnchorCase() {
    InstallMotionFrameRenderer();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-beat-anchor");
    const std::filesystem::path audio = root / "click.wav";
    std::filesystem::create_directories(root);
    bool ok = GenerateClickTrack(audio, 0.5, 64, 48000);
    std::string path;
    std::string error;
    std::string output;
    ok = ok &&
         CreatePortableProject((root / "project").string(),
                               Project::FromDocument(Document{}, "Beat anchor"),
                               path, error);
    ok = ok && IngestCommand(path, audio.string(), false, output) == 0;
    Project project;
    ok = ok && LoadStoredProject(path, project, error) &&
         project.rushes.size() == 1;
    const Ulid music = ok ? project.rushes.front().id : Ulid();
    const auto call = [&](const std::string& tool, const std::string& args,
                          mcp_json::Value& result) {
        mcp_json::Value parsed;
        const bool called = CallToolCommand(path, tool, args, output) == 0 &&
                            mcp_json::Value::Parse(output, parsed, error) &&
                            parsed.Find("result") != nullptr;
        if (called) result = *parsed.Find("result");
        return called;
    };
    mcp_json::Value result;
    ok = ok && call("analyze_audio_beats", "{\"media_id\":\"" + music + "\"}",
                    result);
    std::string videoTrack;
    std::string audioTrack;
    const auto trackOf = [&](const mcp_json::Value& added) {
        const mcp_json::Value* delta = added.Find("delta");
        const mcp_json::Value* created =
            delta != nullptr ? delta->Find("created_tracks") : nullptr;
        const mcp_json::Value* id =
            created != nullptr && created->IsArray() &&
                    !created->AsArray().empty()
                ? created->AsArray().front().Find("track_id")
                : nullptr;
        return id != nullptr && id->IsString() ? id->AsString() : std::string();
    };
    if (ok && call("add_track", R"({"kind":"video"})", result))
        videoTrack = trackOf(result);
    if (ok && call("add_track", R"({"kind":"audio"})", result))
        audioTrack = trackOf(result);
    const auto insert = [&](int64_t timelineIn, int64_t duration) {
        return call("insert_clip",
                    "{\"track_id\":\"" + audioTrack + "\",\"source_id\":\"" +
                        music +
                        "\",\"source_in\":{\"value\":0,\"rate\":25},"
                        "\"duration\":{\"value\":" +
                        std::to_string(duration) +
                        ",\"rate\":25},\"timeline_in\":{\"value\":" +
                        std::to_string(timelineIn) + ",\"rate\":25}}",
                    result);
    };
    ok = ok && !videoTrack.empty() && !audioTrack.empty() && insert(0, 800);
    std::string composition;
    if (ok && call("add_motion_template", R"({"template":"nom"})", result)) {
        const mcp_json::Value* id = result.Find("composition_id");
        if (id != nullptr && id->IsString()) composition = id->AsString();
    }
    if (!ok || composition.empty()) {
        std::cerr << "FAIL: unable to prepare the beat anchor project: "
                  << output << ' ' << error << '\n';
        std::filesystem::remove_all(root);
        return false;
    }
    const auto place = [&](const std::string& beat) {
        CallToolCommand(
            path, "place_motion_clip",
            "{\"composition_id\":\"" + composition + "\",\"track_id\":\"" +
                videoTrack +
                "\",\"props\":{\"nom\":\"Temps\"},\"beat\":" + beat + "}",
            output);
        return output;
    };
    // The frame the engine already names for beat 10, through the clip
    // that plays the music: the card must start there.
    int64_t expected = -1;
    if (call("list_clip_tempo_beats", "{\"media_id\":\"" + music + "\"}",
             result)) {
        const mcp_json::Value* clips = result.Find("clips");
        if (clips != nullptr && clips->IsArray() && !clips->AsArray().empty()) {
            const mcp_json::Value* beats =
                clips->AsArray().front().Find("beats");
            if (beats != nullptr && beats->IsArray())
                for (const mcp_json::Value& beat : beats->AsArray()) {
                    const mcp_json::Value* index = beat.Find("index");
                    const mcp_json::Value* frame = beat.Find("frame");
                    int64_t value = -1;
                    if (index != nullptr && index->AsInt64(value) &&
                        value == 10 && frame != nullptr)
                        frame->AsInt64(expected);
                }
        }
    }
    const std::string onBeat =
        place("{\"media_id\":\"" + music + "\",\"index\":10}");
    if (expected < 0 ||
        onBeat.find("\"timeline_in\":{\"value\":" + std::to_string(expected) +
                    ",\"rate\":25}") == std::string::npos) {
        std::cerr << "FAIL: the card on beat 10 starts on the frame "
                     "list_clip_tempo_beats names ("
                  << expected << "): " << onBeat << '\n';
        ok = false;
    }
    const std::string beyond =
        place("{\"media_id\":\"" + music + "\",\"index\":100000}");
    if (beyond.find("UnknownBeat") == std::string::npos) {
        std::cerr << "FAIL: a beat the grid does not hold is refused by name: "
                  << beyond << '\n';
        ok = false;
    }
    if (!insert(800, 400)) {
        std::cerr << "FAIL: the music is inserted a second time: " << output
                  << '\n';
        ok = false;
    }
    const std::string twice =
        place("{\"media_id\":\"" + music + "\",\"index\":4}");
    // Both frames are named, 800 frames apart: the two playings.
    long long first = -1;
    long long second = -1;
    const size_t listed = twice.find("heard on frames ");
    if (listed != std::string::npos)
        std::sscanf(twice.c_str() + listed, "heard on frames %lld, %lld",
                    &first, &second);
    if (twice.find("AmbiguousBeat") == std::string::npos || first < 0 ||
        second != first + 800) {
        std::cerr << "FAIL: a beat heard twice is refused with both frames: "
                  << twice << '\n';
        ok = false;
    }
    std::filesystem::remove_all(root);
    return ok;
}

int main() {
    try {
        bool ok = true;
        ok &= RunBandEnvelopeCase();
        ok &= RunBeatAnchorCase();
        ok &= RunClickTrackCase(120.0, 16);  // 8s at 120 BPM.
        ok &= RunClickTrackCase(90.0, 12);   // 8s at 90 BPM.
        ok &= RunInvalidSettingsAreRejected();
        ok &= RunCorruptCacheIsRejected();
        ok &= RunProjectToolCase();
        ok &= RunTempoGridToolCase();
        if (!ok) {
            return 1;
        }
        std::cout << "PASS: beat detection cache generated and loaded\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: beat test threw: " << failure.what() << '\n';
        return 1;
    }
}
