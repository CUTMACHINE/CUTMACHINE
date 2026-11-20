#include "BoundaryAir.h"

#include "EditLog.h"
#include "Json.h"
#include "Operations.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"

// B4 -- ROADMAP.md. Synthetic B1 envelopes reproduce the measured failure:
// each half of a junction is below the per-clip threshold, while the silence
// heard across the cut is above it. The old one-side-at-a-time loop needed
// four passes and moved the cut the wrong way; the compound operation below
// measures both original sides, applies once, and is already converged when
// resolved again.

namespace {

constexpr int64_t kRoom = 20000;
constexpr int64_t kVoice = 200000;
const char kSourceA[] = "01K40000000000000000000001";
const char kSourceB[] = "01K40000000000000000000002";

SpeechOnsetReport Envelope(
    const std::string& sourceId,
    const std::vector<std::pair<int64_t, int64_t>>& blocks) {
    SpeechOnsetReport report;
    report.media_id = sourceId;
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    for (const auto& block : blocks) {
        for (int64_t index = 0; index < block.second; ++index)
            report.levels.push_back(block.first);
    }
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    std::string error;
    Check(BuildSpeechGroups(report.levels, report.windows_per_second,
                            report.speech_level, report.noise_floor, {},
                            report.groups, error),
          "les groupes synthétiques se construisent : " + error);
    return report;
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  RationalTime sourceIn, RationalTime duration,
                  RationalTime timelineIn, const std::string& link = {}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = timelineIn;
    clip.link_group_id = link;
    return clip;
}

// The fragment of `track` that starts reading its source at `sourceFrame`
// (25 fps). RemoveWords names new fragments with fresh ULIDs, so a test finds
// them by what they play.
const DocumentClip* FragmentAt(const Document& document,
                               const std::string& trackId,
                               int64_t sourceFrame) {
    const DocumentTrack* track = document.FindTrack(trackId);
    if (track == nullptr) return nullptr;
    for (const DocumentClip& clip : track->clips)
        if (clip.source_in == RationalTime{sourceFrame, 25}) return &clip;
    return nullptr;
}

// No hole on a speech track: it starts at zero and every clip starts where
// the previous one ends.
bool Contiguous(const Document& document, const std::string& trackId) {
    const DocumentTrack* track = document.FindTrack(trackId);
    if (track == nullptr) return false;
    std::vector<DocumentClip> clips = track->clips;
    std::sort(clips.begin(), clips.end(),
              [](const DocumentClip& left, const DocumentClip& right) {
                  return left.timeline_in < right.timeline_in;
              });
    RationalTime end{0, 25};
    for (const DocumentClip& clip : clips) {
        if (clip.timeline_in != end) return false;
        end = clip.timeline_in.add(clip.duration);
    }
    return true;
}

// Whether a fragment reads [sourceIn, sourceIn + duration) at timelineIn,
// all in 25 fps frames.
bool Plays(const DocumentClip* clip, int64_t sourceIn, int64_t duration,
           int64_t timelineIn) {
    return clip != nullptr && clip->source_in == RationalTime{sourceIn, 25} &&
           clip->duration == RationalTime{duration, 25} &&
           clip->timeline_in == RationalTime{timelineIn, 25};
}

}  // namespace

int main() {
    const MediaRate kPal{25, 1};
    std::string error;

    // One clip: both measured edges are removed together, its A/V pair gets
    // identical source bounds, and a third sync-locked track moves by the
    // aggregate delta. Undo must restore canonical bytes, not just geometry.
    {
        Document document;
        document.sources = {
            {kSourceA, "A.wav", kPal, {1000, 25}},
        };
        const std::string videoId = "01K40000000000000000000010";
        const std::string audioId = "01K40000000000000000000011";
        const std::string nextVideoId = "01K40000000000000000000012";
        const std::string nextAudioId = "01K40000000000000000000013";
        const std::string overlayId = "01K40000000000000000000014";
        const std::string link = "01K40000000000000000000020";
        const DocumentClip video =
            Clip(videoId, kSourceA, {0, 25}, {50, 25}, {0, 25}, link);
        const DocumentClip audio =
            Clip(audioId, kSourceA, {0, 25}, {50, 25}, {0, 25}, link);
        document.sequence.tracks = {
            {"01K40000000000000000000030",
             "video",
             0,
             {video,
              Clip(nextVideoId, kSourceA, {100, 25}, {10, 25}, {50, 25})}},
            {"01K40000000000000000000031",
             "audio",
             1,
             {audio,
              Clip(nextAudioId, kSourceA, {100, 25}, {10, 25}, {50, 25})}},
            {"01K40000000000000000000032",
             "video",
             2,
             {Clip(overlayId, kSourceA, {200, 25}, {10, 25}, {50, 25})}},
        };
        const std::string before = document.SaveToString();
        const SpeechOnsetReport envelope =
            Envelope(kSourceA, {{kRoom, 20}, {kVoice, 50}, {kRoom, 30}});
        BoundaryAirSettings settings;
        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(ResolveBoundaryAir(video, envelope, settings, kPal, {}, operation,
                                 report, error),
              "les deux bords se résolvent : " + error);
        Check(operation.trims.size() == 2,
              "la tête et la queue partent dans une seule opération");
        if (operation.trims.size() == 2) {
            Check(operation.trims[0].edge == TrimEdge::Head &&
                      operation.trims[0].delta == RationalTime{7, 25},
                  "trois images sont gardées avant l'attaque");
            Check(operation.trims[1].edge == TrimEdge::Tail &&
                      operation.trims[1].delta == RationalTime{-12, 25},
                  "trois images sont gardées après la décroissance");
        }
        for (BoundaryAirTrim& trim : operation.trims)
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, video, trim.edge);
        operation.sync_track_ids =
            BoundarySyncTrackIds(document, operation.trims);
        Check(operation.sync_track_ids.size() == 1 &&
                  operation.sync_track_ids[0] == "01K40000000000000000000032",
              "la piste sync-lock hors paire suit automatiquement");

        const std::string canonical = SerializeOperation(operation);
        Operation parsed = RemoveClipOperation{};
        EditError editError = EditError::None;
        std::string message;
        Check(DeserializeOperation(canonical, parsed, editError, message) &&
                  SerializeOperation(parsed) == canonical,
              "TrimBoundaryAir se sérialise canoniquement : " + message);

        EditLog log;
        Check(log.Apply(document, operation, editError, message),
              "l'opération de bordure s'applique : " + message);
        const DocumentClip* trimmedVideo = document.FindClip(videoId);
        const DocumentClip* trimmedAudio = document.FindClip(audioId);
        Check(trimmedVideo && trimmedAudio &&
                  trimmedVideo->source_in == RationalTime{7, 25} &&
                  trimmedVideo->duration == RationalTime{31, 25} &&
                  trimmedAudio->source_in == trimmedVideo->source_in &&
                  trimmedAudio->duration == trimmedVideo->duration,
              "la paire A/V perd exactement les mêmes bordures");
        Check(document.FindClip(nextVideoId)->timeline_in ==
                      RationalTime{31, 25} &&
                  document.FindClip(nextAudioId)->timeline_in ==
                      RationalTime{31, 25} &&
                  document.FindClip(overlayId)->timeline_in ==
                      RationalTime{31, 25},
              "toutes les pistes synchronisées avancent de 19 images");
        Check(log.AppliedCount() == 1,
              "les deux bords ne créent qu'une entrée de journal");
        Check(log.Undo(document, editError, message),
              "le geste complet s'annule : " + message);
        Check(document.SaveToString() == before,
              "undo restaure le document octet pour octet");
        Check(log.Redo(document, editError, message),
              "le geste complet se rejoue : " + message);
    }

    // Four-pass divergence fixture: 280 ms on either side is below the
    // 300 ms per-boundary minimum, but the listener hears a 560 ms hole.
    // Individual cleanup does nothing; the junction resolver trims both
    // original sides in one operation and a second resolution is a no-op.
    {
        Document document;
        document.sources = {
            {kSourceA, "left.wav", kPal, {1000, 25}},
            {kSourceB, "right.wav", kPal, {1000, 25}},
        };
        const std::string leftId = "01K40000000000000000000040";
        const std::string rightId = "01K40000000000000000000041";
        const DocumentClip left =
            Clip(leftId, kSourceA, {0, 25}, {32, 25}, {0, 25});
        const DocumentClip right =
            Clip(rightId, kSourceB, {0, 25}, {32, 25}, {32, 25});
        document.sequence.tracks = {
            {"01K40000000000000000000042", "audio", 0, {left, right}},
        };
        const SpeechOnsetReport leftEnvelope =
            Envelope(kSourceA, {{kVoice, 50}, {kRoom, 14}});
        const SpeechOnsetReport rightEnvelope =
            Envelope(kSourceB, {{kRoom, 14}, {kVoice, 50}});
        BoundaryAirSettings settings;
        settings.keep_frames = 2;

        TrimBoundaryAirOperation isolatedLeft;
        TrimBoundaryAirOperation isolatedRight;
        BoundaryAirReport leftReport;
        BoundaryAirReport rightReport;
        Check(ResolveBoundaryAir(left, leftEnvelope, settings, kPal, {},
                                 isolatedLeft, leftReport, error) &&
                  ResolveBoundaryAir(right, rightEnvelope, settings, kPal, {},
                                     isolatedRight, rightReport, error) &&
                  isolatedLeft.trims.empty() && isolatedRight.trims.empty(),
              "chaque demi-creux isolé reste sous le seuil mesuré");

        TrimBoundaryAirOperation operation;
        JunctionAirReport report;
        Check(ResolveJunctionAir(left, leftEnvelope, kPal, right, rightEnvelope,
                                 kPal, settings, {}, operation, report, error),
              "la jonction complète se résout : " + error);
        Check(report.combined_air == RationalTime{28, 50} &&
                  operation.trims.size() == 2,
              "les 560 ms sont mesurées ensemble et les deux côtés partent");

        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(document, operation, editError, message),
              "la fermeture atomique s'applique : " + message);
        const DocumentClip* newLeft = document.FindClip(leftId);
        const DocumentClip* newRight = document.FindClip(rightId);
        Check(newLeft && newRight &&
                  newLeft->duration == RationalTime{27, 25} &&
                  newRight->source_in == RationalTime{5, 25} &&
                  newRight->duration == RationalTime{27, 25} &&
                  newLeft->timeline_in.add(newLeft->duration) ==
                      newRight->timeline_in,
              "queue et tête ferment la jonction sans créer de trou");

        TrimBoundaryAirOperation secondPass;
        JunctionAirReport secondReport;
        Check(ResolveJunctionAir(*newLeft, leftEnvelope, kPal, *newRight,
                                 rightEnvelope, kPal, settings, {}, secondPass,
                                 secondReport, error) &&
                  secondPass.trims.empty() &&
                  secondReport.combined_air == RationalTime{4, 25},
              "une seconde passe est déjà convergée à quatre images gardées");
    }

    // BENCH-2026-09 -- a block cut into fragments the way remove_source_ranges
    // cuts it keeps one link id on every fragment, picture and sound. A
    // boundary trim on one fragment carried the whole group: on a real run a
    // 5-frame head trim on one fragment trimmed the 17 others, leaving 5-frame
    // holes in picture and sound. Each trimmed edge now carries its facing
    // sound only (LinkedEdgePartnerIds, ripple_trim's rule since ITW7).
    {
        Document document;
        document.sources = {{kSourceA, "A.mov", kPal, {1000, 25}}};
        const std::string videoTrack = "01K40000000000000000000070";
        const std::string audioTrack = "01K40000000000000000000071";
        const std::string videoId = "01K40000000000000000000072";
        const std::string audioId = "01K40000000000000000000073";
        const std::string link = "01K40000000000000000000074";
        const DocumentClip block =
            Clip(videoId, kSourceA, {0, 25}, {200, 25}, {0, 25}, link);
        DocumentClip blockSound = block;
        blockSound.id = audioId;
        document.sequence.tracks = {
            {videoTrack, "video", 0, {block}},
            {audioTrack, "audio", 1, {blockSound}},
        };
        // Source frames: speech 0-40, room 40-60, speech 60-100, room
        // 100-120, speech 120-160, room 160-180, speech 180-200.
        const SpeechOnsetReport envelope = Envelope(kSourceA, {{kVoice, 80},
                                                               {kRoom, 40},
                                                               {kVoice, 80},
                                                               {kRoom, 40},
                                                               {kVoice, 80},
                                                               {kRoom, 40},
                                                               {kVoice, 40}});

        // What remove_source_ranges resolves: the ranges, the sound covering
        // them, and the sync tracks the cut does not touch.
        RemoveWordsOperation cut;
        cut.clip_id = videoId;
        cut.ranges = {{{44, 25}, {52, 25}},
                      {{104, 25}, {112, 25}},
                      {{164, 25}, {172, 25}}};
        cut.linked_clip_ids =
            LinkedClipIdsCoveringRanges(document, block, cut.ranges);
        cut.sync_track_ids = WordRemovalSyncTrackIds(document, cut);
        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(document, cut, editError, message),
              "remove_source_ranges découpe le bloc en morceaux : " + message);
        // Hors lot, 29 octobre 2026 -- chaque morceau gardé forme sa paire,
        // comme une coupe dans la fenêtre ; ils gardaient tous le groupe du
        // bloc.
        const auto& pictures = document.FindTrack(videoTrack)->clips;
        const auto& sounds = document.FindTrack(audioTrack)->clips;
        bool pairs = pictures.size() == 4 && sounds.size() == 4;
        std::vector<Ulid> groups;
        for (size_t index = 0; pairs && index < pictures.size(); ++index) {
            pairs =
                !pictures[index].link_group_id.empty() &&
                pictures[index].link_group_id == sounds[index].link_group_id &&
                std::find(groups.begin(), groups.end(),
                          pictures[index].link_group_id) == groups.end();
            groups.push_back(pictures[index].link_group_id);
        }
        Check(pairs && groups.front() == link,
              "chaque morceau forme sa paire ; le premier garde le groupe du "
              "bloc");

        // Fragment 2 reads 52-104: 8 frames (320 ms) of room before speech
        // at 60, 4 frames (160 ms) after it at 100.
        const DocumentClip* second = FragmentAt(document, videoTrack, 52);
        const DocumentClip* secondSound = FragmentAt(document, audioTrack, 52);
        Check(second != nullptr && secondSound != nullptr,
              "le deuxième morceau se retrouve sur les deux pistes");
        if (second == nullptr || secondSound == nullptr) return 1;
        const std::string fragmented = document.SaveToString();

        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(ResolveBoundaryAir(*second, envelope, BoundaryAirSettings{}, kPal,
                                 {}, operation, report, error),
              "les bords du morceau se résolvent : " + error);
        Check(operation.trims.size() == 1 &&
                  operation.trims[0].edge == TrimEdge::Head &&
                  operation.trims[0].delta == RationalTime{5, 25},
              "seule la tête est assez longue : 5 images partent, 3 restent");
        for (BoundaryAirTrim& trim : operation.trims) {
            trim.linked_clip_ids =
                LinkedEdgePartnerIds(document, *second, trim.edge);
        }
        operation.sync_track_ids =
            BoundarySyncTrackIds(document, operation.trims);
        Check(operation.trims.size() == 1 &&
                  operation.trims[0].linked_clip_ids ==
                      std::vector<Ulid>{secondSound->id},
              "le rognage emporte le son en face et rien d'autre");

        const std::string secondId = second->id;
        Check(log.Apply(document, operation, editError, message),
              "le rognage de tête du morceau s'applique : " + message);
        Check(Plays(FragmentAt(document, videoTrack, 0), 0, 44, 0) &&
                  Plays(FragmentAt(document, audioTrack, 0), 0, 44, 0),
              "le premier morceau garde sa tête");
        Check(Plays(FragmentAt(document, videoTrack, 57), 57, 47, 44) &&
                  Plays(FragmentAt(document, audioTrack, 57), 57, 47, 44) &&
                  FragmentAt(document, videoTrack, 57)->id == secondId,
              "le morceau rogné et son partenaire son perdent les mêmes 5 "
              "images");
        Check(
            Plays(FragmentAt(document, videoTrack, 112), 112, 52, 91) &&
                Plays(FragmentAt(document, audioTrack, 112), 112, 52, 91) &&
                Plays(FragmentAt(document, videoTrack, 172), 172, 28, 143) &&
                Plays(FragmentAt(document, audioTrack, 172), 172, 28, 143),
            "les morceaux suivants gardent leur tête et avancent de 5 images");
        Check(Contiguous(document, videoTrack) &&
                  Contiguous(document, audioTrack),
              "aucun trou n'apparaît sur la piste image ni sur la piste son");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == fragmented,
              "undo restaure le bloc découpé octet pour octet : " + message);

        // Junction 2|3: 4 frames of room at the tail of fragment 2, 8 at the
        // head of fragment 3. Both fragments belong to the same group, on the
        // same track: each side carries only its own sound.
        const DocumentClip* third = FragmentAt(document, videoTrack, 112);
        const DocumentClip* thirdSound = FragmentAt(document, audioTrack, 112);
        second = FragmentAt(document, videoTrack, 52);
        secondSound = FragmentAt(document, audioTrack, 52);
        Check(third != nullptr && thirdSound != nullptr && second != nullptr &&
                  secondSound != nullptr,
              "les morceaux de la jonction se retrouvent après undo");
        if (third == nullptr || thirdSound == nullptr || second == nullptr ||
            secondSound == nullptr)
            return 1;
        TrimBoundaryAirOperation junction;
        JunctionAirReport junctionReport;
        Check(ResolveJunctionAir(*second, envelope, kPal, *third, envelope,
                                 kPal, BoundaryAirSettings{}, {}, junction,
                                 junctionReport, error),
              "la jonction entre deux morceaux se résout : " + error);
        Check(junction.trims.size() == 2 &&
                  junction.trims[0].delta == RationalTime{-1, 25} &&
                  junction.trims[1].delta == RationalTime{5, 25},
              "1 image de queue et 5 images de tête partent à la jonction");
        for (BoundaryAirTrim& trim : junction.trims) {
            const DocumentClip* target = document.FindClip(trim.clip_id);
            if (target != nullptr)
                trim.linked_clip_ids =
                    LinkedEdgePartnerIds(document, *target, trim.edge);
        }
        junction.sync_track_ids =
            BoundarySyncTrackIds(document, junction.trims);
        Check(junction.trims.size() == 2 &&
                  junction.trims[0].linked_clip_ids ==
                      std::vector<Ulid>{secondSound->id} &&
                  junction.trims[1].linked_clip_ids ==
                      std::vector<Ulid>{thirdSound->id},
              "chaque côté de la jonction n'emporte que son propre son");
        Check(log.Apply(document, junction, editError, message),
              "la jonction se ferme : " + message);
        Check(Plays(FragmentAt(document, videoTrack, 0), 0, 44, 0) &&
                  Plays(FragmentAt(document, audioTrack, 0), 0, 44, 0) &&
                  Plays(FragmentAt(document, videoTrack, 52), 52, 51, 44) &&
                  Plays(FragmentAt(document, audioTrack, 52), 52, 51, 44) &&
                  Plays(FragmentAt(document, videoTrack, 117), 117, 47, 95) &&
                  Plays(FragmentAt(document, audioTrack, 117), 117, 47, 95) &&
                  Plays(FragmentAt(document, videoTrack, 172), 172, 28, 142) &&
                  Plays(FragmentAt(document, audioTrack, 172), 172, 28, 142),
              "seuls les deux côtés de la jonction et leur son bougent");
        Check(Contiguous(document, videoTrack) &&
                  Contiguous(document, audioTrack),
              "fermer la jonction ne laisse de trou sur aucune piste");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == fragmented,
              "undo de la jonction restaure les octets : " + message);
    }

    // If either constituent trim is invalid, ApplyOperation's candidate copy
    // prevents the first from leaking into the document or the event log.
    {
        Document document;
        document.sources = {
            {kSourceA, "A.wav", kPal, {1000, 25}},
        };
        document.sequence.tracks = {
            {"01K40000000000000000000050",
             "audio",
             0,
             {Clip("01K40000000000000000000051", kSourceA, {0, 25}, {20, 25},
                   {0, 25})}},
        };
        const std::string before = document.SaveToString();
        TrimBoundaryAirOperation invalid{
            {{"01K40000000000000000000051", TrimEdge::Head, {2, 25}, {}},
             {"01K40000000000000000000999", TrimEdge::Tail, {-2, 25}, {}}},
            {},
            {}};
        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(!log.Apply(document, invalid, editError, message),
              "une seconde bordure invalide refuse tout le geste");
        Check(document.SaveToString() == before && log.AppliedCount() == 0,
              "le refus atomique ne laisse ni mutation ni journal");
    }

    // CLI/MCP integers are untrusted. Converting keep_frames to the source
    // timebase refuses before multiplication can wrap an exact duration.
    {
        const SpeechOnsetReport envelope =
            Envelope(kSourceA, {{kRoom, 20}, {kVoice, 50}, {kRoom, 20}});
        BoundaryAirSettings settings;
        settings.keep_frames = std::numeric_limits<int64_t>::max();
        const MediaRate fractionalRate{24000, 1001};
        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(
            !ResolveBoundaryAir(Clip("01K40000000000000000000060", kSourceA,
                                     {0, 25}, {45, 25}, {0, 25}),
                                envelope, settings, fractionalRate, {},
                                operation, report, error) &&
                error.find("exact time range") != std::string::npos,
            "un keep_frames débordant est refusé avant conversion : " + error);
    }

    // AGENT-2026-09 A9 -- a breath after the last word: 15 % of the voice,
    // well above the noise floor, well under the voice. The speech groups
    // heard it as speech and the tail had no air; on the voice line
    // tighten_pauses reads, it is 0.8 s of air, and the tail is trimmed to
    // three frames after the voice.
    {
        constexpr int64_t kBreath = kVoice * 15 / 100;
        const SpeechOnsetReport envelope =
            Envelope(kSourceA, {{kVoice, 50}, {kBreath, 40}, {kRoom, 10}});
        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(ResolveBoundaryAir(Clip("01K40000000000000000000070", kSourceA,
                                      {0, 25}, {50, 25}, {0, 25}),
                                 envelope, BoundaryAirSettings{}, kPal, {},
                                 operation, report, error),
              "un plan finissant sur un souffle se mesure : " + error);
        Check(report.head_air == RationalTime{0, 50} &&
                  report.tail_air == RationalTime{50, 50},
              "le souffle est de l'air, comme pour tighten_pauses : queue " +
                  std::to_string(report.tail_air.value) + "/" +
                  std::to_string(report.tail_air.rate));
        Check(operation.trims.size() == 1 &&
                  operation.trims[0].edge == TrimEdge::Tail &&
                  report.tail_removed == RationalTime{22, 25},
              "la queue est rognée jusqu'à trois images après la voix : " +
                  std::to_string(report.tail_removed.value) + "/" +
                  std::to_string(report.tail_removed.rate));
        // One voiced window in the air ends it, as it does for
        // tighten_pauses: a short burst may be a short word, and a trim past
        // it would cut the word.
        SpeechOnsetReport burst = envelope;
        burst.levels[70] = kVoice;
        Check(ResolveBoundaryAir(Clip("01K40000000000000000000071", kSourceA,
                                      {0, 25}, {50, 25}, {0, 25}),
                                 burst, BoundaryAirSettings{}, kPal, {},
                                 operation, report, error) &&
                  report.tail_air == RationalTime{29, 50},
              "une fenêtre sonore dans l'air borne l'air, comme pour "
              "tighten_pauses : " +
                  std::to_string(report.tail_air.value) + "/" +
                  std::to_string(report.tail_air.rate));
    }

    // Reports are stable JSON values for both headless surfaces.
    {
        BoundaryAirReport report{{10, 25}, {12, 25}, {7, 25}, {9, 25}};
        JunctionAirReport junction{
            {7, 25}, {7, 25}, {14, 25}, {5, 25}, {5, 25}};
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(SerializeBoundaryAirReport(report), parsed,
                                     parseError) &&
                  parsed.IsObject(),
              "le rapport de plan est du JSON valide : " + parseError);
        Check(mcp_json::Value::Parse(SerializeJunctionAirReport(junction),
                                     parsed, parseError) &&
                  parsed.IsObject(),
              "le rapport de jonction est du JSON valide : " + parseError);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests d'air de bordure passent\n";
    return 0;
}
