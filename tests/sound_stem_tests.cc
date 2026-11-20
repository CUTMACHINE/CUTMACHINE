// SN4 -- docs/roadmap/SON-2026-10.md. A stem rendered by cutmachine-sound
// laid at its BWF timecode: the WAV header read, the renderer's rounding
// inverted exactly at 25, 23.976 and 29.97 drop-frame, refusals by name, one
// undo restoring every byte. Portable: the WAV files are written here.

#include "EditLog.h"
#include "Operations.h"
#include "SoundStem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"

namespace {

const Ulid kRush = "01K30000000000000000000S01";
const Ulid kStem = "01K30000000000000000000S02";
const Ulid kVideoTrack = "01K30000000000000000000S03";
const Ulid kAudioTrack = "01K30000000000000000000S04";
const Ulid kShot = "01K30000000000000000000S05";

std::filesystem::path Folder() {
    static const std::filesystem::path folder = [] {
        auto path = std::filesystem::temp_directory_path() /
                    ("cutmachine-sound-stem-" + GenerateUlid());
        std::filesystem::create_directories(path);
        return path;
    }();
    return folder;
}

void Put32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

void Put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>(value >> 8));
}

struct WavSpec {
    int32_t rate = 48000;
    int64_t samples = 0;
    bool bext = true;
    bool bextAfterData = false;
    uint64_t reference = 0;
    std::string riff = "RIFF";
};

// The layout tools/cutmachine-sound/wav.mjs writes: 24-bit stereo PCM, a
// 602-byte bext chunk, silence.
std::string WriteWav(const std::string& name, const WavSpec& spec) {
    std::string bext = "bext";
    Put32(bext, 602);
    std::string body(602, '\0');
    for (int index = 0; index < 4; ++index) {
        body[338 + index] =
            static_cast<char>((spec.reference >> (8 * index)) & 0xff);
        body[342 + index] =
            static_cast<char>((spec.reference >> (32 + 8 * index)) & 0xff);
    }
    body[346] = 1;
    bext += body;
    std::string fmt = "fmt ";
    Put32(fmt, 16);
    Put16(fmt, 1);
    Put16(fmt, 2);
    Put32(fmt, static_cast<uint32_t>(spec.rate));
    Put32(fmt, static_cast<uint32_t>(spec.rate) * 6);
    Put16(fmt, 6);
    Put16(fmt, 24);
    std::string data = "data";
    Put32(data, static_cast<uint32_t>(spec.samples * 6));
    data += std::string(static_cast<size_t>(spec.samples * 6), '\0');
    std::string chunks;
    if (spec.bext && !spec.bextAfterData) {
        chunks += bext;
    }
    chunks += fmt + data;
    if (spec.bext && spec.bextAfterData) {
        chunks += bext;
    }
    std::string file = spec.riff;
    Put32(file, static_cast<uint32_t>(chunks.size() + 4));
    file += "WAVE" + chunks;
    const std::string path = (Folder() / name).string();
    std::ofstream(path, std::ios::binary) << file;
    return path;
}

// A timeline at `rate` with one shot of `shotFrames` on V1, an empty audio
// track, and the stem in the library, `samples` long at `sampleRate`.
Document Film(const MediaRate& rate, int64_t shotFrames, int32_t sampleRate,
              int64_t samples) {
    Document document;
    document.sequence.id = "01K30000000000000000000S06";
    document.sequence.frame_rate = rate;
    const RationalTime rushLength{int64_t{1000} * rate.den, rate.num};
    document.sources = {
        {kRush, "rush.mov", rate, rushLength},
        {kStem, "Sound/stem.wav", {sampleRate, 1}, {samples, sampleRate}},
    };
    LibraryMedia rush;
    rush.id = kRush;
    rush.path = "rush.mov";
    rush.filename = "rush.mov";
    rush.rate = rate;
    rush.duration = rushLength;
    rush.codec = "h264";
    rush.width = 320;
    rush.height = 180;
    rush.orientation = "landscape";
    LibraryMedia stem;
    stem.id = kStem;
    stem.path = "Sound/stem.wav";
    stem.filename = "stem.wav";
    stem.codec = "pcm_s24le";
    stem.has_video = false;
    stem.orientation = "audio";
    stem.has_audio = true;
    stem.audio_rate = sampleRate;
    stem.audio_channels = 2;
    stem.rate = {sampleRate, 1};
    stem.duration = {samples, sampleRate};
    document.library = {rush, stem};
    DocumentClip shot{kShot,
                      kRush,
                      {0, rate.num},
                      {shotFrames * rate.den, rate.num},
                      {0, rate.num}};
    shot.include_audio = false;
    DocumentTrack video;
    video.id = kVideoTrack;
    video.kind = "video";
    video.index = 0;
    video.clips = {shot};
    DocumentTrack audio;
    audio.id = kAudioTrack;
    audio.kind = "audio";
    audio.index = 1;
    document.sequence.tracks = {video, audio};
    return document;
}

bool Place(const Document& document, const SoundStemRequest& request,
           BatchOperation& batch, SoundStemPlacement& placement,
           EditError& error, std::string& message) {
    return BuildSoundStemPlacement(document, request, batch, placement, error,
                                   message);
}

void TestReadBroadcastWave() {
    BroadcastWave wave;
    std::string error;
    const uint64_t far = (uint64_t{3} << 32) + 7;
    Check(
        ReadBroadcastWave(WriteWav("far.wav", {44100, 1234, true, false, far}),
                          wave, error) &&
            wave.sample_rate == 44100 && wave.channels == 2 &&
            wave.samples == 1234 && wave.has_time_reference &&
            wave.time_reference == static_cast<int64_t>(far),
        "the bext time reference reads both words: " + error);
    Check(ReadBroadcastWave(
              WriteWav("after.wav", {48000, 10, true, true, 172800000}), wave,
              error) &&
              wave.has_time_reference && wave.time_reference == 172800000,
          "a bext chunk after the data is read: " + error);
    Check(ReadBroadcastWave(WriteWav("plain.wav", {48000, 10, false}), wave,
                            error) &&
              !wave.has_time_reference && wave.samples == 10,
          "a WAV without bext reads, without a time reference: " + error);
    Check(!ReadBroadcastWave(
              WriteWav("rf64.wav", {48000, 10, true, false, 0, "RF64"}), wave,
              error) &&
              error.find("RF64") != std::string::npos,
          "RF64 is refused by name: " + error);
    const std::string text = (Folder() / "text.wav").string();
    std::ofstream(text) << "not a wave file at all";
    Check(!ReadBroadcastWave(text, wave, error) &&
              error.find("not a RIFF/WAVE file") != std::string::npos,
          "a file that is not a WAV is refused: " + error);
    Check(!ReadBroadcastWave((Folder() / "absent.wav").string(), wave, error),
          "a missing file is refused");
}

void TestFrameSample() {
    const MediaRate ntsc{24000, 1001};
    Check(SoundStemFrameSample(40, ntsc, 44100) == 73574,
          "frame 40 at 23.976 is sample 73573.5, a half rounding up");
    Check(SoundStemFrameSample(86400, ntsc, 48000) == 172972800,
          "01:00:00:00 at 23.976 and 48 kHz");
    Check(SoundStemFrameSample(107892, {30000, 1001}, 48000) == 172799827,
          "01:00:00;00 at 29.97 is sample 172799827.2, rounded down");
    Check(SoundStemFrameSample(90000, {25, 1}, 48000) == 172800000,
          "01:00:00:00 at 25 and 48 kHz");
}

void TestPlacement25() {
    // 100 frames rendered with a 0.3 s tail: 4 s + 14400 samples.
    const int64_t samples = 100 * 1920 + 14400;
    Document document = Film({25, 1}, 100, 48000, samples);
    SoundStemRequest request;
    request.media_id = kStem;
    std::string error;
    Check(ReadBroadcastWave(
              WriteWav("stem25.wav", {48000, samples, true, false, 172800000}),
              request.wave, error),
          "the stem reads: " + error);
    BatchOperation batch;
    SoundStemPlacement placement;
    EditError editError = EditError::None;
    std::string message;
    Check(Place(document, request, batch, placement, editError, message),
          "a stem rendered from 01:00:00:00 is laid: " + message);
    Check(placement.first_frame == 0 && placement.frames == 107 &&
              placement.timeline_in == RationalTime(0, 25) &&
              placement.duration == RationalTime(107, 25) &&
              placement.track_added,
          "on the timeline's first frame, over its 107 whole frames, on a "
          "new track");
    Check(placement.not_laid == RationalTime(960, 48000),
          "the last half frame of the tail is not laid");

    const std::string before = document.SaveToString();
    EditLog log;
    Check(log.Apply(document, batch, editError, message),
          "the batch applies: " + message);
    const DocumentTrack* track = document.FindTrack(placement.track_id);
    const DocumentClip* clip = document.FindClip(placement.clip_id);
    Check(track != nullptr && track->kind == "audio" && track->index == 2 &&
              clip != nullptr && clip->source_id == kStem &&
              clip->timeline_in == placement.timeline_in &&
              clip->duration == placement.duration &&
              clip->source_in == RationalTime(0, 1),
          "a new audio track above every other holds the stem");
    const std::string after = document.SaveToString();
    Check(log.Undo(document, editError, message) &&
              document.SaveToString() == before,
          "one undo restores every byte: " + message);
    Check(log.Redo(document, editError, message) &&
              document.SaveToString() == after,
          "redo lays the same clip, same ids: " + message);

    // On a named audio track, a second time: the paste refuses to cover it.
    request.track_id = placement.track_id;
    Check(Place(document, request, batch, placement, editError, message) &&
              !placement.track_added,
          "a named track is used as it is: " + message);
    const std::string laid = document.SaveToString();
    Check(!log.Apply(document, batch, editError, message) &&
              editError == EditError::Overlap &&
              document.SaveToString() == laid,
          "a stem over a stem is refused whole: " + message);

    // A later start timecode moves it later.
    request.track_id = kAudioTrack;
    request.start_timecode = "00:59:58:00";
    Check(Place(document, request, batch, placement, editError, message) &&
              placement.first_frame == 50,
          "rendered for 01:00:00:00, laid at 00:59:58:00 start: frame 50");
}

void TestPlacementNtsc() {
    // The fixture stem of tools/cutmachine-sound at 44.1 kHz: 234 frames,
    // 430405 samples, from 01:00:00:00.
    const MediaRate ntsc{24000, 1001};
    Document document = Film(ntsc, 234, 44100, 430405);
    SoundStemRequest request;
    request.media_id = kStem;
    request.track_id = kAudioTrack;
    std::string error;
    Check(
        ReadBroadcastWave(WriteWav("stem2398.wav",
                                   {44100, 430405, true, false,
                                    static_cast<uint64_t>(SoundStemFrameSample(
                                        86400, ntsc, 44100))}),
                          request.wave, error),
        "the 23.976 stem reads: " + error);
    BatchOperation batch;
    SoundStemPlacement placement;
    EditError editError = EditError::None;
    std::string message;
    Check(Place(document, request, batch, placement, editError, message) &&
              placement.first_frame == 0 && placement.frames == 234 &&
              placement.duration == RationalTime(int64_t{234} * 1001, 24000) &&
              placement.not_laid ==
                  RationalTime(
                      int64_t{430405} * 24000 - int64_t{234} * 1001 * 44100,
                      44100 * 24000),
          "the whole 234 frames, from the first: " + message);
    Check(batch.operations.size() == 1, "no track added on a named one");

    // 176 frames end on sample 323723.4 at 44.1 kHz: the renderer rounds a
    // stem's length up so that it holds its last frame; one sample less
    // does not hold it, and only the frames held whole are laid.
    for (const auto& [samples, frames] :
         std::vector<std::pair<int64_t, int64_t>>{{323724, 176},
                                                  {323723, 175}}) {
        Document shortFilm = Film(ntsc, 176, 44100, samples);
        request.wave.samples = samples;
        Check(Place(shortFilm, request, batch, placement, editError, message) &&
                  placement.frames == frames,
              std::to_string(samples) + " samples hold " +
                  std::to_string(frames) + " whole frames: " + message);
    }
    request.wave.samples = 430405;

    // A time reference one sample off is on no frame: refused, not moved.
    request.wave.time_reference += 1;
    Check(!Place(document, request, batch, placement, editError, message) &&
              editError == EditError::InvalidTimelineIn &&
              message.find("on no frame of a 24000/1001 timeline") !=
                  std::string::npos,
          "a stem off the frames is refused: " + message);
    request.wave.time_reference -= 1;
    request.start_timecode = "10:00:00:00";
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("frames before the timeline's first frame") !=
                  std::string::npos,
          "a stem before the timeline is refused: " + message);
}

void TestPlacementDropFrame() {
    const MediaRate rate{30000, 1001};
    const int64_t samples = int64_t{48000} * 2;
    Document document = Film(rate, 60, 48000, samples);
    SoundStemRequest request;
    request.media_id = kStem;
    request.start_timecode = "01:00:00;00";
    request.wave = {48000, 2, samples, true,
                    SoundStemFrameSample(107892, rate, 48000)};
    BatchOperation batch;
    SoundStemPlacement placement;
    EditError editError = EditError::None;
    std::string message;
    Check(Place(document, request, batch, placement, editError, message) &&
              placement.first_frame == 0 && placement.frames == 59,
          "29.97 drop-frame: on the first frame, 59 whole frames in 2 s: " +
              message);
}

void TestRefusals() {
    Document document = Film({25, 1}, 100, 48000, 192000);
    SoundStemRequest request;
    request.media_id = kStem;
    request.wave = {48000, 2, 192000, true, 172800000};
    BatchOperation batch;
    SoundStemPlacement placement;
    EditError editError = EditError::None;
    std::string message;

    request.media_id = "01K30000000000000000000S99";
    Check(!Place(document, request, batch, placement, editError, message) &&
              editError == EditError::UnknownMedia,
          "an unknown media is refused");
    request.media_id = kRush;
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("has a picture") != std::string::npos,
          "a rush with a picture is refused: " + message);
    request.media_id = kStem;
    request.wave.has_time_reference = false;
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("carries no BWF timecode") != std::string::npos,
          "a WAV without bext is refused: " + message);
    request.wave.has_time_reference = true;
    request.wave.samples = 191999;
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("not what the library holds") != std::string::npos,
          "a file that changed since its ingest is refused: " + message);
    request.wave.samples = 192000;
    request.track_id = kVideoTrack;
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("not an audio track") != std::string::npos,
          "a video track is refused: " + message);
    request.track_id = kAudioTrack;
    document.FindTrack(kAudioTrack)->locked = true;
    Check(!Place(document, request, batch, placement, editError, message) &&
              editError == EditError::LockedTrack,
          "a locked track is refused");
    document.FindTrack(kAudioTrack)->locked = false;
    request.start_timecode = "01:00:00:30";
    Check(!Place(document, request, batch, placement, editError, message) &&
              message.find("start timecode '01:00:00:30'") != std::string::npos,
          "an impossible start timecode is refused: " + message);
    request.start_timecode = kEdlDefaultStartTimecode;
    request.wave.samples = 1000;
    document.sources[1].duration = {1000, 48000};
    Check(!Place(document, request, batch, placement, editError, message) &&
              editError == EditError::InvalidDuration,
          "less than a frame is refused: " + message);
}

}  // namespace

int main() {
    TestReadBroadcastWave();
    TestFrameSample();
    TestPlacement25();
    TestPlacementNtsc();
    TestPlacementDropFrame();
    TestRefusals();
    std::filesystem::remove_all(Folder());
    if (failures != 0) {
        std::cerr << failures << " sound stem check(s) failed\n";
        return 1;
    }
    std::cout << "sound stem tests passed\n";
    return 0;
}
