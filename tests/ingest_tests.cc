#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Ingest.h"
#include "MediaSource.h"
#include "Operations.h"
#include "ProjectStorage.h"
#include "ResolveImport.h"
#include "Ulid.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

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
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-ingest");
    const std::filesystem::path mediaDirectory = root / "media";
    const std::filesystem::path rawPath = root / "raw.mp4";
    const std::filesystem::path variableRatePath = root / "variable-rate.mp4";
    const std::filesystem::path alphaPath = root / "transparent.mov";
    const std::filesystem::path audioPath = root / "voice.wav";
    const std::filesystem::path videoPath = mediaDirectory / "rotated.mp4";
    std::filesystem::create_directories(mediaDirectory);

    std::string error;
    std::string projectPath;
    Check(CreatePortableProject((root / "Ingest.cutmachine-project").string(),
                                Project("Ingest"), projectPath, error),
          "empty project package saves: " + error);
    const std::filesystem::path documentPath = projectPath;

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=64x32:r=30000/1001:d=1.001' "
        "-f lavfi -i 'sine=frequency=1000:sample_rate=48000:duration=1.001' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(rawPath);
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg must generate the media fixture");
    const std::string rotate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -display_rotation:v:0 90 -i " +
        Quote(rawPath) + " -c copy " + Quote(videoPath);
    Check(std::system(rotate.c_str()) == 0,
          "FFmpeg must attach a display matrix");
    const std::string generateVariableRate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=64x32:rate=30:duration=1' "
        "-vf \"select='not(eq(n,15))'\" -fps_mode vfr -c:v mpeg4 " +
        Quote(variableRatePath);
    Check(std::system(generateVariableRate.c_str()) == 0,
          "FFmpeg must generate the variable-rate media fixture");
    const std::string generateAlpha =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi "
        "-i 'color=c=red@0.5:s=16x16:r=25:d=0.2,format=rgba' "
        "-c:v qtrle -pix_fmt argb " +
        Quote(alphaPath);
    Check(std::system(generateAlpha.c_str()) == 0,
          "FFmpeg must generate the transparent video fixture");
    const std::string generateAudio =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:a pcm_s16le " +
        Quote(audioPath);
    Check(std::system(generateAudio.c_str()) == 0,
          "FFmpeg must generate the audio-only fixture");
    MediaSource alphaSource;
    Check(alphaSource.Open(alphaPath.string()),
          "transparent video decode source opens");
    const AVFrame* alphaFrame = nullptr;
    int64_t alphaPts = 0;
    Check(alphaSource.DecodeFrame(0, alphaFrame, alphaPts) && alphaFrame,
          "transparent video frame decodes");
    if (alphaFrame) {
        const AVPixFmtDescriptor* pixel =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(alphaFrame->format));
        Check(pixel && pixel->nb_components == 4 &&
                  (pixel->flags & AV_PIX_FMT_FLAG_ALPHA) &&
                  pixel->comp[0].plane != pixel->comp[1].plane &&
                  pixel->comp[0].plane != pixel->comp[2].plane &&
                  pixel->comp[3].plane != pixel->comp[0].plane,
              "packed source alpha is normalized to four Metal-ready planes");
        if (pixel && pixel->nb_components == 4) {
            const int alphaPlane = pixel->comp[3].plane;
            const uint16_t alpha = reinterpret_cast<const uint16_t*>(
                alphaFrame->data[alphaPlane])[0];
            Check(alpha > 30000 && alpha < 36000,
                  "the normalized alpha plane preserves 50 percent coverage");
        }
    }
    LibraryMedia variableRateMetadata;
    Check(ProbeMediaMetadata(variableRatePath.string(), variableRateMetadata,
                             error),
          "variable-rate metadata probe succeeds: " + error);
    MediaSource variableRateSource;
    Check(variableRateSource.Open(variableRatePath.string()),
          "variable-rate decode source opens");
    Check(variableRateMetadata.rate.num ==
                  variableRateSource.FrameRateNumerator() &&
              variableRateMetadata.rate.den ==
                  variableRateSource.FrameRateDenominator(),
          "ingest and decode retain the same exact variable frame rate");
    Check(variableRateSource.ActiveThreadType() == FF_THREAD_FRAME,
          "a long-GOP stream keeps frame threading");

    // FI14 -- docs/roadmap/FIABILITE-2026-10.md. A song with its cover art
    // is a sound: the attached picture is neither its picture nor a reason
    // to refuse it.
    const std::filesystem::path coveredPath = root / "covered.m4a";
    const std::string generateCovered =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-f lavfi -i 'color=c=red:s=32x32:d=0.04' -map 0:a -map 1:v "
        "-c:a aac -c:v png -frames:v 1 -disposition:v:0 attached_pic " +
        Quote(coveredPath);
    Check(std::system(generateCovered.c_str()) == 0,
          "FFmpeg must generate the song with its cover art");
    LibraryMedia covered;
    Check(ProbeMediaMetadata(coveredPath.string(), covered, error),
          "a song with cover art is ingested: " + error);
    Check(!covered.has_video && covered.has_audio &&
              covered.orientation == "audio" && covered.rate.num == 48000 &&
              covered.duration.value > 0,
          "its cover art is not taken for a picture");

    // PERF10 -- docs/roadmap/PERF-2026-09.md. An intra-only stream -- the
    // H.264 High 4:2:2 Intra a camera's XAVC-I rushes carry, or ProRes --
    // decodes with slice threading, which costs a third of the memory of
    // frame threading on a 4K rush and seeks five times faster; the picture
    // has to be the very same.
    for (const auto& [name, codec] :
         {std::pair<std::string, std::string>{
              "intra.mp4",
              "-c:v libx264 -profile:v high422 -pix_fmt yuv422p10le "
              "-x264-params keyint=1:slices=4"},
          {"intra.mov", "-c:v prores_ks -profile:v 2"}}) {
        const std::filesystem::path intraPath = root / name;
        const std::string generateIntra =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi "
            "-i 'testsrc2=s=128x64:r=25:d=0.4' " +
            codec + " " + Quote(intraPath);
        Check(std::system(generateIntra.c_str()) == 0,
              "FFmpeg must generate the intra-only fixture " + name);
        MediaSource sliced;
        MediaSource framed;
        Check(sliced.Open(intraPath.string(), 5) &&
                  framed.Open(intraPath.string(), 5, FF_THREAD_FRAME),
              name + " opens with either threading");
        Check(sliced.ActiveThreadType() == FF_THREAD_SLICE,
              name + " is decoded with slice threading by default");
        const AVFrame* slicedFrame = nullptr;
        const AVFrame* framedFrame = nullptr;
        int64_t slicedPts = 0;
        int64_t framedPts = 0;
        const bool decoded = sliced.DecodeFrame(6, slicedFrame, slicedPts) &&
                             framed.DecodeFrame(6, framedFrame, framedPts) &&
                             slicedFrame && framedFrame;
        bool same = decoded && slicedPts == framedPts &&
                    slicedFrame->format == framedFrame->format;
        const AVPixFmtDescriptor* pixel =
            same ? av_pix_fmt_desc_get(
                       static_cast<AVPixelFormat>(slicedFrame->format))
                 : nullptr;
        for (int plane = 0;
             same && pixel && plane < 4 && slicedFrame->data[plane] != nullptr;
             ++plane) {
            const int rows =
                plane == 0 || (pixel->flags & AV_PIX_FMT_FLAG_RGB) || plane == 3
                    ? slicedFrame->height
                    : AV_CEIL_RSHIFT(slicedFrame->height, pixel->log2_chroma_h);
            const int bytes = av_image_get_linesize(
                static_cast<AVPixelFormat>(slicedFrame->format),
                slicedFrame->width, plane);
            for (int row = 0; same && row < rows; ++row)
                same = std::equal(
                    slicedFrame->data[plane] + static_cast<ptrdiff_t>(row) *
                                                   slicedFrame->linesize[plane],
                    slicedFrame->data[plane] +
                        static_cast<ptrdiff_t>(row) *
                            slicedFrame->linesize[plane] +
                        bytes,
                    framedFrame->data[plane] +
                        static_cast<ptrdiff_t>(row) *
                            framedFrame->linesize[plane]);
        }
        Check(same, name +
                        " frame 6 is the same picture whichever threading "
                        "decoded it");
    }

    {
        std::ofstream text(mediaDirectory / "notes.txt");
        text << "not media\n";
    }
    {
        std::ofstream corrupt(mediaDirectory / "corrupt.mp4", std::ios::binary);
        corrupt << "broken mp4";
    }
    // What a Mac writes next to every file on an exFAT drive, and its folder
    // litter: passed over and counted, never reported as a failed media.
    {
        std::ofstream appleDouble(mediaDirectory / "._rotated.mp4",
                                  std::ios::binary);
        appleDouble << std::string("\x00\x05\x16\x07", 4) << "Mac OS X";
        std::ofstream finder(mediaDirectory / ".DS_Store", std::ios::binary);
        finder << "Bud1";
    }

    std::string output;
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "mixed folder ingest succeeds: " + output);
    Check(output.find("\"added\":1") != std::string::npos &&
              output.find("\"skipped\":2") != std::string::npos,
          "one media is added and two unreadable files are skipped");
    Check(output.find("notes.txt") != std::string::npos &&
              output.find("corrupt.mp4") != std::string::npos,
          "both unreadable files have reported reasons");
    Check(output.find("\"ignored\":2") != std::string::npos &&
              output.find("._rotated") == std::string::npos &&
              output.find("DS_Store") == std::string::npos,
          "AppleDouble and Finder files are counted as ignored, not "
          "reported: " +
              output);
    Check(IsVolumeLitter("/Volumes/CARD/._C1500.MP4") &&
              !IsVolumeLitter("/Volumes/CARD/C1500.MP4") &&
              !IsVolumeLitter("/Volumes/CARD/.hidden.mov") &&
              IsVolumeSystemDirectory("/Volumes/CARD/.Spotlight-V100") &&
              IsVolumeSystemDirectory(
                  "/Volumes/CARD/System Volume Information") &&
              !IsVolumeSystemDirectory("/Volumes/CARD/.rushes"),
          "volume litter is recognised by name, and only the known names");

    Project ingestedProject;
    Check(LoadStoredProject(documentPath.string(), ingestedProject, error),
          "ingested project loads: " + error);
    Document ingested = ingestedProject.MakeActiveDocument();
    Check(ingested.library.size() == 1, "library contains one media");
    if (ingested.library.size() == 1) {
        const LibraryMedia& media = ingested.library[0];
        Check(media.rate.num == 30000 && media.rate.den == 1001,
              "avg_frame_rate remains the exact 30000/1001 rational");
        Check(media.width == 64 && media.height == 32,
              "stored dimensions remain the coded dimensions");
        Check(!media.pixel_format.empty() && !media.color_range.empty() &&
                  !media.color_space.empty() && !media.color_transfer.empty() &&
                  !media.color_primaries.empty(),
              "pixel format and color signalling flow through ingest");
        Check(media.orientation == "portrait",
              "display-matrix rotation controls orientation");
        Check(std::abs(media.rotation_degrees) == 90,
              "display-matrix rotation is retained for presentation");
        Check(media.has_audio && media.audio_rate == 48000 &&
                  media.audio_channels == 1,
              "audio header metadata is extracted");
        // QC-2026-09 A3 -- FFmpeg's own volumedetect reads this fixture's
        // AAC audio at -21.1 dBFS, which is 88000 on the kAudioLevelScale
        // grid. The band below is that figure with room for the mono downmix
        // and for a different encoder build, and is still far from anything
        // else the measurement could mean. What the ingest has to get right
        // is the order of magnitude, because that is what decides whether
        // Whisper runs.
        Check(media.audio_level_measured,
              "ingest measures the mean audio level as a document fact");
        Check(media.audio_level > 50000 && media.audio_level < 150000,
              "the measured level lands where FFmpeg's own reading of the "
              "same audio does: " +
                  std::to_string(media.audio_level));
        Check(media.audio_level >= kSilentMediaAudioLevel,
              "and is not mistaken for a mute cutaway");
        const DocumentSource* source = ingested.FindSource(media.id);
        Check(source && source->rate.num == media.rate.num &&
                  source->rate.den == media.rate.den &&
                  source->duration == media.duration,
              "ingest creates a source with the same stable media ULID");
        // IA10 -- a created project already holds a video and an audio track.
        Check(ingested.sequence.tracks.size() == 2 &&
                  ingested.sequence.tracks[0].kind == "video",
              "the created timeline can take an ingested media as is");
        Operation insert = InsertClipOperation{ingested.sequence.tracks[0].id,
                                               media.id,
                                               {0, media.duration.rate},
                                               media.duration,
                                               {0, media.duration.rate},
                                               {},
                                               {}};
        Operation inverse = RemoveClipOperation{};
        EditError editError = EditError::None;
        std::string editMessage;
        Check(ApplyOperation(ingested, insert, inverse, editError, editMessage),
              "an ingested media can be inserted directly: " + editMessage);
    }

    // PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The same folder, the way the
    // window ingests it: the files read by the engine's first half (what the
    // window's background task runs), then added to a document it has not
    // written yet, into a bin. The rush is the entry --ingest wrote above --
    // its audio level with it, which the window's own loop never measured --
    // save for its id and for the bin, which only the window says.
    {
        Document windowDocument = Project("Window").MakeActiveDocument();
        DocumentBin bin;
        bin.name = "Rushes";
        windowDocument.bins.push_back(bin);
        const std::filesystem::path projectDirectory =
            documentPath.parent_path();
        std::set<std::string> settled =
            CompleteLibraryPaths(windowDocument, projectDirectory);
        std::vector<ProbedIngestFile> dropped;
        for (const std::filesystem::path& file :
             {videoPath, mediaDirectory / "corrupt.mp4", audioPath}) {
            dropped.push_back(ProbeIngestFile(file, settled));
        }
        const IngestOutcome outcome =
            AddProbedMedia(windowDocument, projectDirectory, dropped, bin.id);
        Check(outcome.added_ids.size() == 2 && outcome.skipped == 1 &&
                  outcome.errors.size() == 1 &&
                  outcome.errors[0].file == "corrupt.mp4" && outcome.changed &&
                  windowDocument.Validate(error),
              "the window's ingest adds the two media and names the file it "
              "cannot read: " +
                  error);
        // A library entry as the document format writes it, every field of
        // it, under a fixed id.
        const auto written = [&](LibraryMedia media) {
            Document holder = windowDocument;
            media.id = "01K70000000000000000000000";
            holder.library = {std::move(media)};
            holder.sources.clear();
            return holder.SaveToString();
        };
        if (ingested.library.size() == 1 &&
            windowDocument.library.size() == 2) {
            const LibraryMedia& byCommand = ingested.library[0];
            LibraryMedia byWindow = windowDocument.library[0];
            Check(byWindow.id != byCommand.id && byWindow.bin_id == bin.id &&
                      byWindow.path == byCommand.path,
                  "the window's entry has its own id, the bin it was dropped "
                  "on, and the path --ingest stores");
            Check(byWindow.audio_level_measured &&
                      byWindow.audio_level == byCommand.audio_level,
                  "a rush ingested by the window has its audio level "
                  "measured, the same figure: " +
                      std::to_string(byWindow.audio_level));
            byWindow.bin_id.clear();
            Check(written(byWindow) == written(byCommand),
                  "a rush ingested by the window is the library entry "
                  "--ingest writes, to the id and the bin");
            const DocumentSource* source =
                windowDocument.FindSource(windowDocument.library[0].id);
            Check(source != nullptr && source->path == byCommand.path &&
                      source->duration == byCommand.duration,
                  "and has its source");
        } else {
            Check(false, "both ingests hold the rush to compare");
        }
        // Dropped again: nothing is decoded for a file the library holds.
        settled = CompleteLibraryPaths(windowDocument, projectDirectory);
        const ProbedIngestFile again = ProbeIngestFile(videoPath, settled);
        const std::string beforeAgain = windowDocument.SaveToString();
        const IngestOutcome second =
            AddProbedMedia(windowDocument, projectDirectory, {again}, bin.id);
        Check(!again.media && again.error.empty() && !second.changed &&
                  second.skipped == 1 && second.moved == 0 &&
                  windowDocument.SaveToString() == beforeAgain,
              "a rush dropped again on its bin is not read and changes "
              "nothing");
    }

    // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The same rush brought
    // in from a Resolve manifest has its audio level measured too, the
    // figure --ingest measured: it was the third way in, and the one that
    // read headers only.
    {
        const std::filesystem::path resolveRoot = root / "resolve-level";
        std::string resolvePath;
        Check(CreatePortableProject(
                  (resolveRoot / "Resolve.cutmachine-project").string(),
                  Project("Resolve"), resolvePath, error),
              "the Resolve project saves: " + error);
        const std::filesystem::path manifest = resolveRoot / "manifest.json";
        {
            std::ofstream out(manifest);
            out << R"({"schema":"cutmachine.resolve-manifest.v1",)"
                << R"("project":"Resolve","bins":[)"
                << R"({"key":"b1","name":"1_RUSHES","parent_key":""}],)"
                << R"("clips":[{"path":")" << videoPath.string()
                << R"(","name":"rotated.mp4","bin_key":"b1"}]})";
        }
        Check(ImportResolveCommand(resolvePath, manifest.string(), output) == 0,
              "--import-resolve brings the sound rush in: " + output);
        Project imported;
        std::string loadError;
        Check(LoadStoredProject(resolvePath, imported, loadError) &&
                  imported.rushes.size() == 1,
              "the Resolve project holds the rush: " + loadError);
        if (imported.rushes.size() == 1 && ingested.library.size() == 1) {
            const LibraryMedia& byResolve = imported.rushes[0];
            Check(byResolve.audio_level_measured &&
                      byResolve.audio_level == ingested.library[0].audio_level,
                  "a rush imported from Resolve has its audio level, the "
                  "figure --ingest measured: " +
                      std::to_string(byResolve.audio_level));
        }
    }

    // DEC32 (PRD7) -- an ingest is one step of the active timeline's
    // journal, into the bin it names: undone and redone like any edit, a
    // known rush filed by a second ingest into another bin, an unknown bin
    // refused before anything is decoded or written.
    {
        const std::filesystem::path journalRoot = root / "ingest-journal";
        std::string journalPath;
        Check(CreatePortableProject(
                  (journalRoot / "Journal.cutmachine-project").string(),
                  Project("Journal"), journalPath, error),
              "the project saves: " + error);
        const Ulid rushesBin = "01K7DEC3200000000000000001";
        const Ulid selectsBin = "01K7DEC3200000000000000002";
        for (const auto& [id, name] :
             {std::pair<Ulid, std::string>{rushesBin, "Rushes"},
              {selectsBin, "Selects"}}) {
            Check(ApplyOperationCommand(journalPath,
                                        "{\"type\":\"AddBin\",\"bin_id\":\"" +
                                            id + "\",\"name\":\"" + name +
                                            "\",\"parent_id\":\"\"}",
                                        output) == 0,
                  "bin " + name + ": " + output);
        }
        const auto stored = [&] {
            Project loaded;
            std::string loadError;
            Check(LoadStoredProject(journalPath, loaded, loadError),
                  "the project reads back: " + loadError);
            return loaded;
        };
        const std::string empty = Read(journalPath);
        Check(
            IngestCommand(journalPath, videoPath.string(), false, output,
                          Ulid("01K7DEC3200000000000000099")) == 1 &&
                output.find("\"error\":\"UnknownBin\"") != std::string::npos &&
                Read(journalPath) == empty,
            "an unknown bin is refused, nothing written: " + output);
        Check(IngestCommand(journalPath, videoPath.string(), false, output,
                            rushesBin) == 0 &&
                  output.find("\"added\":1") != std::string::npos,
              "the rush comes in, into Rushes: " + output);
        const std::string ingestedBytes = Read(journalPath);
        Project project = stored();
        Check(project.rushes.size() == 1 &&
                  project.rushes[0].bin_id == rushesBin &&
                  project.rushes[0].audio_level_measured,
              "in its bin, with its level");
        EditLog log;
        EditError editError = EditError::None;
        Check(EditLog::Load(TimelineEditLogPathForProject(
                                journalPath, project.active_timeline_id),
                            log, editError, error) &&
                  log.AppliedCount() == 3 &&
                  std::holds_alternative<IngestMediaOperation>(
                      log.AppliedEntries().back().op),
              "the ingest is the journal's last entry, after the two bins: " +
                  error);
        Check(
            UndoOperationCommand(journalPath, output) == 0 &&
                stored().rushes.empty() && Read(journalPath) == empty,
            "undo takes the ingest back, the project byte for byte: " + output);
        Check(RedoOperationCommand(journalPath, output) == 0 &&
                  Read(journalPath) == ingestedBytes,
              "redo brings it again, byte for byte: " + output);
        Check(IngestCommand(journalPath, videoPath.string(), false, output,
                            selectsBin) == 0 &&
                  output.find("\"added\":0") != std::string::npos &&
                  stored().rushes[0].bin_id == selectsBin,
              "ingested again into Selects, the known rush moves there: " +
                  output);
        Check(UndoOperationCommand(journalPath, output) == 0 &&
                  Read(journalPath) == ingestedBytes,
              "and undo puts it back in Rushes: " + output);
    }

    // B11 -- a precise file path is an ingest root too, and a sound file is
    // first-class media. Its sample clock becomes the exact source timebase;
    // no synthetic black video is needed.
    Check(IngestCommand(documentPath.string(), audioPath.string(), false,
                        output) == 0,
          "a single audio file ingests directly: " + output);
    Check(output.find("\"added\":1") != std::string::npos &&
              output.find("no video stream") == std::string::npos,
          "audio-only ingest succeeds instead of reporting a video refusal");
    Project audioProject;
    Check(LoadStoredProject(documentPath.string(), audioProject, error),
          "project with audio-only media loads: " + error);
    const auto audioMedia =
        std::find_if(audioProject.rushes.begin(), audioProject.rushes.end(),
                     [&](const LibraryMedia& media) {
                         return media.filename == audioPath.filename().string();
                     });
    Check(audioMedia != audioProject.rushes.end(),
          "audio-only media is present in the library");
    if (audioMedia != audioProject.rushes.end()) {
        Check(!audioMedia->has_video && audioMedia->has_audio,
              "audio-only capability is explicit in the library");
        Check(audioMedia->width == 0 && audioMedia->height == 0 &&
                  audioMedia->orientation == "audio",
              "audio-only media carries no fabricated picture metadata");
        Check(audioMedia->rate.num == 48000 && audioMedia->rate.den == 1 &&
                  audioMedia->duration == RationalTime{48000, 48000},
              "audio samples provide the exact rate and duration");
        const std::string canonical =
            audioProject.MakeActiveDocument().SaveToString();
        Check(canonical.find("\"has_video\":false") != std::string::npos,
              "canonical library JSON exposes has_video false");

        Document audioDocument = audioProject.MakeActiveDocument();
        Operation audioInsert =
            InsertClipOperation{audioDocument.sequence.tracks[1].id,
                                audioMedia->id,
                                {0, 48000},
                                audioMedia->duration,
                                {0, 48000},
                                {},
                                {}};
        Operation audioInverse = RemoveClipOperation{};
        EditError audioEditError = EditError::None;
        std::string audioEditMessage;
        Check(
            ApplyOperation(audioDocument, audioInsert, audioInverse,
                           audioEditError, audioEditMessage),
            "audio-only media inserts on an audio track: " + audioEditMessage);
        Operation videoInsert =
            InsertClipOperation{audioDocument.sequence.tracks[0].id,
                                audioMedia->id,
                                {0, 48000},
                                audioMedia->duration,
                                {0, 48000},
                                {},
                                {}};
        Check(!ApplyOperation(audioDocument, videoInsert, audioInverse,
                              audioEditError, audioEditMessage) &&
                  audioEditError == EditError::InvalidOperation,
              "audio-only media is refused explicitly on a video track");

        std::string qualityOutput;
        Check(AnalyzeShotQualityCommand(documentPath.string(), audioMedia->id,
                                        qualityOutput) == 1 &&
                  qualityOutput.find("\"error\":\"InvalidOperation\"") !=
                      std::string::npos &&
                  qualityOutput.find("audio-only") != std::string::npos,
              "shot-quality refuses audio-only media before decoding");
    }

    // QC-2026-09 A3 -- the case the ticket is about: a mute cutaway. 29 of one
    // project's 71 rushes were this, and each went to Whisper at eleven times
    // its own runtime before anyone noticed.
    {
        const std::filesystem::path silentPath = root / "silent.mp4";
        const std::string generateSilent =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error "
            "-f lavfi -i 'color=c=black:s=64x32:r=25:d=1' "
            "-f lavfi -i 'anullsrc=sample_rate=48000:channel_layout=mono' "
            "-c:v mpeg4 -c:a aac -shortest " +
            Quote(silentPath);
        Check(std::system(generateSilent.c_str()) == 0,
              "FFmpeg must generate the mute fixture");
        int64_t level = -1;
        std::string levelReason;
        Check(MeasureMediaAudioLevel(silentPath.string(), FFMPEG_EXECUTABLE,
                                     level, levelReason),
              "a mute media still measures: " + levelReason);
        Check(level < kSilentMediaAudioLevel,
              "digital silence falls under the threshold that skips Whisper: " +
                  std::to_string(level));

        // A media with no audio stream at all is a refusal with a reason, not
        // a zero a caller would read as "measured, and silent".
        int64_t missing = -1;
        Check(!MeasureMediaAudioLevel(variableRatePath.string(),
                                      FFMPEG_EXECUTABLE, missing, levelReason),
              "a media without an audio stream reports that it cannot be "
              "measured");
    }

    // G3 -- docs/roadmap/GRADE-2026-09.md. The input a file declares, read
    // at ingest: its tags when they name one, nothing when they do not, and
    // nothing either when the container names a curve of its own over them,
    // as a Blackmagic Pocket 4K does over its Film log tagged BT.709.
    {
        const auto tagged = [&](const std::string& name,
                                const std::string& params,
                                const std::string& extra) {
            const std::filesystem::path path = root / name;
            const std::string command =
                Quote(FFMPEG_EXECUTABLE) +
                " -hide_banner -loglevel error -f lavfi -i "
                "'color=c=gray:s=64x32:r=25:d=0.2,setparams=" +
                params + "' -c:v mpeg4 " + extra + ' ' + Quote(path);
            Check(std::system(command.c_str()) == 0,
                  "FFmpeg must generate " + name);
            return path;
        };
        const std::string bt709 =
            "color_primaries=bt709:color_trc=bt709:colorspace=bt709";
        const auto phone = tagged("phone.mov", bt709, "");
        const auto pocket =
            tagged("pocket.mov", bt709,
                   "-movflags use_metadata_tags -metadata "
                   "com.apple.proapps.customgamma=com.blackmagic-design.camera."
                   "filmlog");
        const auto hlg = tagged(
            "hlg.mov",
            "color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc",
            "");
        LibraryMedia probed;
        std::string probeError;
        Check(ProbeMediaMetadata(phone.string(), probed, probeError) &&
                  probed.color_input.transfer == "rec709" &&
                  probed.color_input.gamut == "rec709" &&
                  probed.color_input.ycbcr_matrix == "auto" &&
                  probed.color_input.range == "auto",
              "a BT.709-tagged file declares a Rec.709 input: " + probeError);
        Check(ProbeMediaMetadata(pocket.string(), probed, probeError) &&
                  probed.color_transfer == "bt709" &&
                  !DeclaresColorInput(probed.color_input),
              "tags a custom gamma contradicts declare nothing");
        Check(ProbeMediaMetadata(hlg.string(), probed, probeError) &&
                  probed.color_input.transfer == "hlg" &&
                  probed.color_input.gamut == "rec2020",
              "HLG tags declare an HLG input");
        Check(ProbeMediaMetadata(rawPath.string(), probed, probeError) &&
                  !DeclaresColorInput(probed.color_input) &&
                  probed.white_balance_kelvin == 0,
              "an untagged file follows the project");
        Check(ProbeMediaMetadata(audioPath.string(), probed, probeError) &&
                  !DeclaresColorInput(probed.color_input),
              "a sound declares no input");
        MediaFileColor facts;
        Check(ReadFileColorFacts(hlg.string(), facts, probeError) &&
                  facts.input.transfer == "hlg" &&
                  facts.white_balance_kelvin == 0,
              "the facts read again are the ingest's");
        Check(!ReadFileColorFacts((root / "missing.mov").string(), facts,
                                  probeError),
              "a missing file cannot be read again");

        // COLOR-T -- « à chaque fois qu'on importe des rushes ça doit
        // rester cohérent quelle que soit la méthode d'import » (the author,
        // 4 October 2026). One rush that declares its input, brought into
        // three projects by the three imports there are -- --ingest, which
        // ingest_media runs; the window's two halves; Resolve's Media Pool
        // -- is read alike by all three, as the probe reads it.
        const std::filesystem::path parity = root / "parity";
        std::filesystem::create_directories(parity / "rushes");
        const std::filesystem::path rush = parity / "rushes" / "C9001.MOV";
        std::filesystem::copy_file(hlg, rush);
        LibraryMedia reference;
        Check(ProbeMediaMetadata(rush.string(), reference, probeError) &&
                  DeclaresColorInput(reference.color_input),
              "the parity rush declares its input: " + probeError);
        const auto created = [&](const std::string& name) {
            std::string path;
            Check(CreatePortableProject(
                      (parity / (name + ".cutmachine-project")).string(),
                      Project(name), path, error),
                  name + " saves: " + error);
            return path;
        };
        const auto stored = [&](const std::string& path) {
            Project loaded;
            std::string loadError;
            Check(LoadStoredProject(path, loaded, loadError) &&
                      loaded.rushes.size() == 1,
                  "the imported project holds the rush: " + loadError);
            return loaded.rushes.empty() ? LibraryMedia{} : loaded.rushes[0];
        };
        const std::string byCommandPath = created("Commande");
        Check(IngestCommand(byCommandPath, rush.string(), false, output) == 0,
              "--ingest brings the rush in: " + output);
        const LibraryMedia byCommand = stored(byCommandPath);

        Document windowDocument = Project("Fenetre").MakeActiveDocument();
        const std::filesystem::path windowDirectory = parity / "Fenetre";
        std::set<std::string> settled =
            CompleteLibraryPaths(windowDocument, windowDirectory);
        AddProbedMedia(windowDocument, windowDirectory,
                       {ProbeIngestFile(rush, settled)}, std::nullopt);
        const LibraryMedia byWindow = windowDocument.library.empty()
                                          ? LibraryMedia{}
                                          : windowDocument.library[0];

        const std::string byResolvePath = created("Resolve");
        const std::filesystem::path manifest = parity / "manifest.json";
        {
            std::ofstream out(manifest);
            out << R"({"schema":"cutmachine.resolve-manifest.v1",)"
                << R"("project":"Resolve","bins":[)"
                << R"({"key":"b1","name":"1_RUSHES","parent_key":""}],)"
                << R"("clips":[{"path":")" << rush.string()
                << R"(","name":"C9001.MOV","bin_key":"b1"}]})";
        }
        Check(
            ImportResolveCommand(byResolvePath, manifest.string(), output) == 0,
            "--import-resolve brings the rush in: " + output);
        const LibraryMedia byResolve = stored(byResolvePath);

        for (const auto& [how, media] :
             {std::pair<const char*, const LibraryMedia*>{"--ingest",
                                                          &byCommand},
              {"the window", &byWindow},
              {"--import-resolve", &byResolve}}) {
            Check(
                media->white_balance_kelvin == reference.white_balance_kelvin &&
                    media->color_input == reference.color_input,
                std::string(how) +
                    " reads the rush's picture as the probe does");
        }
    }

    const std::string beforeSecondIngest = Read(documentPath);
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "second ingest succeeds: " + output);
    Check(output.find("\"added\":0") != std::string::npos &&
              Read(documentPath) == beforeSecondIngest,
          "second ingest is byte-idempotent");

    std::string description;
    Check(DescribeCommand(documentPath.string(), description) == 0,
          "describe succeeds after ingest");
    // PRD39 -- the command line's "ok" in front of the snapshot.
    Check(description.find("{\"ok\":true,\"sequence\":") == 0 &&
              description.find("\"timeline\":") != std::string::npos &&
              description.find("\"library\":[") != std::string::npos &&
              description.find("\"alias\":\"M1\"") != std::string::npos &&
              description.find("\"in_use\":false") != std::string::npos,
          "describe separates timeline and library with media aliases");

    const std::string beforeFailedScan = Read(documentPath);
    Check(IngestCommand(documentPath.string(), (root / "missing").string(),
                        false, output) == 1 &&
              Read(documentPath) == beforeFailedScan,
          "global scan failure leaves the document byte-identical");

    // PRD20 -- ROADMAP.md. An ingest that read nothing is a refusal: a file
    // that is not media used to answer ok:true and status 0, the failure
    // only in `errors`. The mixed folder above stays a success, on its first
    // ingest (one media added) as on its second (one media known).
    Check(IngestCommand(documentPath.string(),
                        (mediaDirectory / "notes.txt").string(), false,
                        output) == 1 &&
              output.find("\"ok\":false") != std::string::npos &&
              output.find("\"error\":\"IoError\"") != std::string::npos &&
              output.find("nothing was ingested: 'notes.txt' could not be "
                          "read (") != std::string::npos &&
              output.find("\"result\":{\"added\":0,\"skipped\":1,"
                          "\"ignored\":0,\"errors\":[{\"file\":"
                          "\"notes.txt\",\"reason\":\"") != std::string::npos &&
              Read(documentPath) == beforeFailedScan,
          "a file that is not media is a refused ingest, its report under "
          "result: " +
              output);
    const std::filesystem::path unreadable = root / "unreadable";
    std::filesystem::create_directory(unreadable);
    std::filesystem::copy_file(mediaDirectory / "notes.txt",
                               unreadable / "notes.txt");
    std::filesystem::copy_file(mediaDirectory / "corrupt.mp4",
                               unreadable / "corrupt.mp4");
    std::filesystem::copy_file(mediaDirectory / "._rotated.mp4",
                               unreadable / "._rotated.mp4");
    Check(
        IngestCommand(documentPath.string(), unreadable.string(), false,
                      output) == 1 &&
            output.find("\"error\":\"IoError\"") != std::string::npos &&
            output.find("nothing was ingested: none of the 2 files could "
                        "be read, starting with 'corrupt.mp4' (") !=
                std::string::npos &&
            output.find("\"skipped\":2,\"ignored\":1,") != std::string::npos &&
            output.find("notes.txt") != std::string::npos &&
            Read(documentPath) == beforeFailedScan,
        "a folder where every file fails is a refused ingest that names "
        "them all: " +
            output);

    std::filesystem::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All ingest tests passed\n";
    return 0;
}
