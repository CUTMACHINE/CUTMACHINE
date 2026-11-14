// PERF13 -- AudioDecode.h. The monitor's sound reads the sound of a rush and
// nothing else, and it is the same sound.
//
// Each fixture interleaves a heavy picture with its sound, the way a camera
// writes a rush, and is compared with the very same sound packets remuxed
// alone: whatever surrounds them, the samples decoded must not change, and
// reading them must not cost the picture.

#include "AudioDecode.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string()) {
        result += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return result + "'";
}

bool Run(const std::string& arguments) {
    const std::string command = Quote(FFMPEG_EXECUTABLE) +
                                " -hide_banner -loglevel error -y " + arguments;
    return std::system(command.c_str()) == 0;
}

// The picture: uncompressed 4:2:2 at 640x360, 460,800 bytes a frame, so
// four seconds weigh 46 MB against 768 KB of 16-bit stereo -- the
// proportion of a 4K intra rush to its sound, within a factor of two.
const char kPicture[] = "-f lavfi -i testsrc=size=640x360:rate=25:duration=4 ";

// The sound decoded whole from `media`, and from the same packets copied
// into a file of their own: equal sample for sample.
void CheckSameSound(const std::filesystem::path& media,
                    const std::filesystem::path& alone, int32_t rate,
                    int64_t expectedSamples, bool stereoSource,
                    const std::string& label) {
    if (!Run("-i " + Quote(media) + " -map 0:a -c copy " + Quote(alone))) {
        Check(false, label + ": the sound alone is remuxed");
        return;
    }
    audio_decode::StereoPcm16 interleaved;
    audio_decode::StereoPcm16 separate;
    audio_decode::DecodeStats stats;
    std::string error;
    Check(audio_decode::DecodeStereoPcm16(media.string(), rate, interleaved,
                                          error, nullptr, &stats),
          label + ": the rush decodes: " + error);
    Check(
        audio_decode::DecodeStereoPcm16(alone.string(), rate, separate, error),
        label + ": its sound alone decodes: " + error);
    Check(!interleaved.left.empty() &&
              interleaved.left.size() == interleaved.right.size(),
          label + ": stereo samples come out");
    const int64_t samples = static_cast<int64_t>(interleaved.left.size());
    Check(samples >= expectedSamples - rate / 10 &&
              samples <= expectedSamples + rate / 10,
          label + ": the whole duration, at the mix rate: " +
              std::to_string(samples));
    Check(interleaved.left == separate.left &&
              interleaved.right == separate.right,
          label + ": the same samples as the same sound without its picture");
    // A stereo source keeps its two channels apart; a mono one is the same
    // signal on both sides.
    Check((interleaved.left != interleaved.right) == stereoSource,
          label + (stereoSource ? ": the two channels stay distinct"
                                : ": the mono channel feeds both sides"));
    // The stream headers are probed as before, a few megabytes at most;
    // the picture packets after them are never read.
    const auto size = static_cast<int64_t>(std::filesystem::file_size(media));
    std::cout << label << ": read " << stats.bytes_read << " of " << size
              << " bytes\n";
    Check(stats.bytes_read > 0 && stats.bytes_read < size / 4,
          label + ": the decode reads the sound, not the picture: " +
              std::to_string(stats.bytes_read) + " of " + std::to_string(size) +
              " bytes");
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cutmachine-audio-decode-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);

    // The camera's case: big-endian 16-bit PCM in QuickTime, interleaved in
    // blocks, a different tone in each channel.
    const std::filesystem::path pcm = root / "pcm.mov";
    if (Run(std::string(kPicture) +
            "-f lavfi -i "
            "'aevalsrc=sin(440*2*PI*t)|0.5*sin(660*2*PI*t):s=48000:d=4' "
            "-map 0:v -map 1:a -c:v rawvideo -pix_fmt uyvy422 "
            "-c:a pcm_s16be -shortest " +
            Quote(pcm))) {
        CheckSameSound(pcm, root / "pcm-alone.mov", 48000, int64_t{4} * 48000,
                       true, "PCM 48 kHz");
    } else {
        Check(false, "the PCM fixture is generated");
    }

    // A phone's case: AAC, mono at 44.1 kHz, upmixed and resampled.
    const std::filesystem::path aac = root / "aac.mov";
    if (Run(std::string(kPicture) +
            "-f lavfi -i 'sine=frequency=330:sample_rate=44100:duration=4' "
            "-map 0:v -map 1:a -c:v rawvideo -pix_fmt uyvy422 "
            "-c:a aac -ac 1 -shortest " +
            Quote(aac))) {
        CheckSameSound(aac, root / "aac-alone.mov", 48000, int64_t{4} * 48000,
                       false, "AAC 44.1 kHz mono");
    } else {
        Check(false, "the AAC fixture is generated");
    }

    // A picture without sound says so, and reads no further than its
    // headers.
    const std::filesystem::path silent = root / "silent.mov";
    if (Run(std::string(kPicture) + "-c:v rawvideo -pix_fmt uyvy422 " +
            Quote(silent))) {
        audio_decode::StereoPcm16 output;
        std::string error;
        Check(!audio_decode::DecodeStereoPcm16(silent.string(), 48000, output,
                                               error) &&
                  error == "no audio stream",
              "a rush without sound is answered as such: " + error);
    } else {
        Check(false, "the silent fixture is generated");
    }

    // A decode the caller abandons stops at its first packet.
    {
        std::atomic_bool cancel{true};
        audio_decode::StereoPcm16 output;
        std::string error;
        Check(!audio_decode::DecodeStereoPcm16(pcm.string(), 48000, output,
                                               error, &cancel) &&
                  error == "audio decode cancelled",
              "a cancelled decode stops: " + error);
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures == 0) {
        std::cout << "audio_decode: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
