#include "TimelineAudioReport.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// ITW8 -- ROADMAP.md. The assembled sound of a timeline, measured against
// the rushes that play it.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-timeline-audio-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    // 0.4 s of room, then a voice-level tone with a quiet hollow from 1.0 to
    // 1.6 s, until 2.6 s.
    const auto source = directory / "voice.wav";
    const std::string generate =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error -f lavfi -i "
        "\"aevalsrc='if(lt(t,0.4)+between(t,1,1.6),0.001,0.3)*sin(2*PI*440*t)'"
        ":s=48000:d=2.6\" -c:a pcm_s16le -y '" +
        source.string() + "'";
    Check(std::system(generate.c_str()) == 0, "the voice source is generated");

    Document document;
    const Ulid sourceId = "01K4AUD0000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = source.string();
    media.filename = "voice.wav";
    media.metadata_complete = true;
    media.has_audio = true;
    media.has_video = false;
    media.rate = {25, 1};
    media.duration = {65, 25};
    document.library = {media};
    document.sources = {{sourceId, source.string(), {25, 1}, {65, 25}}};
    DocumentTrack audio{"01K4AUD0000000000000000002", "audio", 0, {}};
    // The whole take, a 15-frame gap, then its first second again.
    audio.clips = {{GenerateUlid(), sourceId, {0, 25}, {65, 25}, {0, 25}},
                   {GenerateUlid(), sourceId, {10, 25}, {25, 25}, {80, 25}}};
    document.sequence.tracks = {audio};

    SpeechOnsetReport onset;
    onset.media_id = sourceId;
    onset.noise_floor = 707;      // -63 dBFS
    onset.speech_level = 212132;  // -13.5 dBFS
    std::map<Ulid, SpeechOnsetReport> floors{{sourceId, onset}};
    TimelineAudioSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    TimelineAudioReport report;
    std::string error;
    Check(AnalyzeTimelineAudio(document, directory / "project.json", floors,
                               settings, report, error),
          "the timeline audio is measured: " + error);
    bool hollow = false;
    bool gap = false;
    for (const TimelineAudioDip& dip : report.dips) {
        const double start =
            static_cast<double>(dip.start.value) / dip.start.rate;
        const double end = static_cast<double>(dip.end.value) / dip.end.rate;
        if (dip.kind == "hollow" && start > 0.95 && start < 1.05 &&
            end > 1.55 && end < 1.65 && dip.threshold_dbfs &&
            *dip.threshold_dbfs == -38)
            hollow = true;
        if (dip.kind == "gap" && start > 2.55 && start < 2.65 && end > 3.15 &&
            end < 3.25 && !dip.threshold_dbfs)
            gap = true;
    }
    Check(hollow, "the quiet stretch under the rush's midpoint is a hollow");
    Check(gap, "the stretch where no clip plays is a gap");
    Check(report.head_air && report.head_air->value >= 38 &&
              report.head_air->value <= 42 && report.head_air->rate == 100,
          "the air before the first voice is the 0.4 s of room");
    Check(report.integrated_lufs && report.true_peak_dbfs &&
              *report.true_peak_dbfs < -2.0 && report.head_peak_dbfs <= -3,
          "loudness and peaks are read after the export's limiter");
    Check(report.duration == RationalTime{105, 25} &&
              report.unmeasured_windows == 0 &&
              report.sources_without_floor.empty(),
          "every window of the timeline is judged");

    TimelineAudioReport unfloored;
    Check(AnalyzeTimelineAudio(document, directory / "project.json", {},
                               settings, unfloored, error) &&
              unfloored.sources_without_floor.size() == 1 &&
              unfloored.unmeasured_windows > 0 &&
              std::none_of(unfloored.dips.begin(), unfloored.dips.end(),
                           [](const TimelineAudioDip& dip) {
                               return dip.kind == "hollow";
                           }),
          "a rush without a floor is named and never judged hollow");

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All timeline audio report tests passed\n";
    return 0;
}
