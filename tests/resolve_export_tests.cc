#include "Document.h"
#include "Json.h"
#include "ResolveExport.h"
#include "ResolveSend.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "TestSupport.h"

// PA16 -- docs/roadmap/PARITE-2026-11.md. The descriptor of the one send
// that carries a whole montage to Resolve (schema v3). What each field has
// to be was measured on Resolve 21.1.0.17 on 13 November 2026; the header of
// src/media/ResolveExport.h says how.

namespace {

LibraryMedia Media(const std::string& id, const std::string& name,
                   int32_t rateNum) {
    LibraryMedia media;
    media.id = id;
    media.filename = name;
    media.path = "/rushes/" + name;
    media.codec = "h264";
    media.width = 3840;
    media.height = 2160;
    media.orientation = "landscape";
    media.rate = {rateNum, 1};
    media.duration = {1000, rateNum};
    return media;
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  RationalTime timelineIn, RationalTime sourceIn,
                  RationalTime duration) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.timeline_in = timelineIn;
    clip.source_in = sourceIn;
    clip.duration = duration;
    return clip;
}

// R2 -- docs/roadmap/REEL-2026-09.md. Two keys on one clip, the smallest
// animation the document accepts.
std::vector<ClipParameterAnimation> ZoomTrack() {
    ClipKeyframe first;
    first.id = "01K30000000000000000000K01";
    first.time = {0, 25};
    first.value = EffectParamValue{1, 1};
    ClipKeyframe last;
    last.id = "01K30000000000000000000K02";
    last.time = {12, 25};
    last.value = EffectParamValue{23, 20};
    ClipParameterAnimation track;
    track.parameter = ClipAnimatedParameter::FramingScale;
    track.keyframes = {first, last};
    return {track};
}

const ResolveSendLoss* FindLoss(const ResolveTimelineExport& exported,
                                const std::string& what) {
    for (const ResolveSendLoss& loss : exported.losses)
        if (loss.what == what) return &loss;
    return nullptr;
}

bool Same(const ResolveXmlFraction& value, int64_t num, int64_t den) {
    return static_cast<__int128>(value.num) * den ==
           static_cast<__int128>(num) * value.den;
}

constexpr const char* kA = "01K30000000000000000000001";
constexpr const char* kB = "01K30000000000000000000002";
constexpr const char* kMusic = "01K30000000000000000000003";
constexpr const char* kTalk = "01K30000000000000000000020";
constexpr const char* kCut = "01K30000000000000000000021";
constexpr const char* kVoice = "01K30000000000000000000022";

// A talking shot, then a cutaway after it, on V1; the talking shot's own
// sound on A1. 2160x3840 is a 9:16 sequence; the rushes are UHD 16:9.
Document Montage() {
    Document document;
    document.sequence.name = "MONTAGE";
    document.sequence.frame_rate = {25, 1};
    document.sequence.width = 2160;
    document.sequence.height = 3840;
    document.library = {Media(kA, "A.MP4", 25), Media(kB, "B.MP4", 25)};
    DocumentTrack video;
    video.id = "01K30000000000000000000010";
    video.kind = "video";
    video.index = 0;
    // Volontairement dans le désordre : c'est la position sur la timeline qui
    // fait foi, pas l'ordre de stockage.
    video.clips = {Clip(kCut, kB, {100, 25}, {50, 25}, {25, 25}),
                   Clip(kTalk, kA, {0, 25}, {0, 25}, {100, 25})};
    video.clips[1].link_group_id = "01K300000000000000000000L1";
    DocumentTrack audio;
    audio.id = "01K30000000000000000000011";
    audio.kind = "audio";
    audio.index = 1;
    audio.clips = {Clip(kVoice, kA, {0, 25}, {0, 25}, {100, 25})};
    audio.clips[0].link_group_id = "01K300000000000000000000L1";
    document.sequence.tracks = {video, audio};
    return document;
}

DocumentClip& ClipById(Document& document, const std::string& id) {
    for (DocumentTrack& track : document.sequence.tracks)
        for (DocumentClip& clip : track.clips)
            if (clip.id == id) return clip;
    static DocumentClip none;
    return none;
}

const ResolveTimelineClip* SentClip(const ResolveTimelineExport& exported,
                                    const std::string& id) {
    for (const ResolveTimelineClip& clip : exported.clips)
        if (clip.clip_id == id) return &clip;
    return nullptr;
}

}  // namespace

int main() {
    // --- Conversion exacte ------------------------------------------------
    int64_t frames = -1;
    Check(ExactFrameCount({100, 25}, {25, 1}, frames) && frames == 100,
          "une position au débit de la source donne son numéro d'image");
    Check(ExactFrameCount({100, 25}, {50, 1}, frames) && frames == 200,
          "la même seconde vaut deux fois plus d'images à 50 i/s");
    // La moitié d'une image à 25 i/s : Resolve ne sait pas l'exprimer, et
    // arrondir déplacerait la coupe.
    Check(!ExactFrameCount({1, 50}, {25, 1}, frames),
          "une position entre deux images est refusée, jamais arrondie");
    Check(!ExactFrameCount({10, 0}, {25, 1}, frames),
          "un débit nul est refusé");

    std::string error;

    // --- Les plans, image et son -----------------------------------------
    {
        const Document document = Montage();
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(document, exported, error),
              "un montage simple se décrit : " + error);
        Check(exported.name == "MONTAGE" && exported.width == 2160 &&
                  exported.height == 3840 && exported.duration == 125,
              "le format et la longueur de la timeline sont transmis");
        Check(exported.clips.size() == 3,
              "le son part comme un plan à lui : 2 plans image, 1 plan son");
        Check(exported.clips.size() == 3 &&
                  exported.clips[0].clip_id == kTalk &&
                  exported.clips[1].clip_id == kCut &&
                  exported.clips[2].clip_id == kVoice,
              "l'image d'abord, dans l'ordre de la timeline, puis le son");
        if (exported.clips.size() == 3) {
            const ResolveTimelineClip& talk = exported.clips[0];
            // 100 images à partir de 0 : la borne de fin est exclusive, parce
            // que Resolve construit end-start images.
            Check(!talk.audio && talk.track == 1 && talk.start_frame == 0 &&
                      talk.end_frame == 100 && talk.record_frame == 0 &&
                      talk.duration == 100,
                  "un plan image porte sa piste, sa plage et sa position");
            Check(exported.clips[1].start_frame == 50 &&
                      exported.clips[1].end_frame == 75 &&
                      exported.clips[1].record_frame == 100,
                  "le second plan porte sa propre plage source");
            const ResolveTimelineClip& voice = exported.clips[2];
            Check(voice.audio && voice.track == 1 && voice.start_frame == 0 &&
                      voice.end_frame == 100 && !voice.framed,
                  "le plan son va sur A1 avec sa plage, sans cadrage");
            Check(talk.link_group == 0 && voice.link_group == 0 &&
                      exported.clips[1].link_group == -1,
                  "l'image et le son d'une même séparation sont reliés");
            // Une image UHD 16:9 dans un 9:16 : Resolve la met à l'échelle
            // selon le projet. Ajustée, le zoom vaut 1 ; remplie, le rapport
            // des deux échelles, 0,5625 / 1,777... = 81/256.
            Check(talk.framed && Same(talk.fit.zoom, 1, 1) &&
                      Same(talk.fit.pan, 0, 1) && Same(talk.fit.tilt, 0, 1) &&
                      Same(talk.fill.zoom, 81, 256),
                  "un plan sans cadrage reste ajusté dans les deux réglages");
        }
        Check(exported.tracks.size() == 2 && !exported.tracks[0].audio &&
                  exported.tracks[1].audio && exported.tracks[1].mono,
              "une piste son qui ne porte que le son des rushes passe en "
              "mono (consigne du monteur)");
        Check(exported.losses.empty(), "rien n'est perdu");
        Check(exported.transitions.empty() && exported.markers.empty() &&
                  exported.subtitle_cues == 0,
              "ni transition, ni marqueur, ni sous-titre");

        const std::string json = SerializeResolveTimelineExport(exported);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(json, parsed, parseError),
              "le descripteur est du JSON : " + parseError);
        Check(json.find("\"schema\":\"cutmachine.resolve-timeline.v3\"") !=
                  std::string::npos,
              "le descripteur annonce le schéma v3");
        Check(json.find("\"kind\":\"audio\"") != std::string::npos &&
                  json.find("\"link_group\":0") != std::string::npos &&
                  json.find("\"zoom\":0.316406") != std::string::npos &&
                  json.find("\"mono\":true") != std::string::npos &&
                  json.find("speed_percent") == std::string::npos &&
                  json.find("\"grade\":\"none\"") != std::string::npos,
              "le descripteur porte les plans son, les liens, le cadrage "
              "décimal, la piste mono ; aucune vitesse : " +
                  json);
    }

    // --- Recouvrement -----------------------------------------------------
    {
        Document layered = Montage();
        DocumentTrack overlay;
        overlay.id = "01K30000000000000000000012";
        overlay.kind = "video";
        overlay.index = 2;
        overlay.clips = {Clip("01K30000000000000000000023", kB, {25, 25},
                              {200, 25}, {50, 25})};
        overlay.visible = false;
        layered.sequence.tracks.push_back(overlay);
        ResolveTimelineExport covered;
        Check(BuildResolveTimelineExport(layered, covered, error),
              "un montage à deux pistes vidéo se décrit : " + error);
        const ResolveTimelineClip* cut =
            SentClip(covered, "01K30000000000000000000023");
        Check(cut != nullptr && cut->track == 2 && cut->record_frame == 25,
              "le plan de coupe nomme sa piste et sa position");
        Check(covered.tracks.size() == 3 && !covered.tracks[1].enabled,
              "une piste vidéo masquée arrive désactivée");
    }

    // --- Vitesse ------------------------------------------------------------
    {
        // SetSpeed garde la position et la durée : un ralenti de 50 images
        // se pose sur 50 images de source, puis prend sa vitesse et n'en lit
        // que 25.
        Document slow = Montage();
        DocumentClip& talk = ClipById(slow, kTalk);
        talk.duration = {50, 25};
        talk.source_in = {200, 25};
        talk.speed = {1, 2};
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(slow, exported, error),
              "un ralenti se décrit : " + error);
        const ResolveTimelineClip* sent = SentClip(exported, kTalk);
        Check(sent != nullptr && sent->start_frame == 200 &&
                  sent->end_frame == 250 && sent->duration == 50 &&
                  Same(sent->speed, 1, 2),
              "le ralenti part sur sa longueur à vitesse 1, puis 50 %");
        const std::string json = SerializeResolveTimelineExport(exported);
        Check(json.find("\"speed_percent\":50") != std::string::npos,
              "la vitesse sort en pourcentage : " + json);

        // Trop près de la fin du rush : à vitesse 1, la pose lirait au-delà.
        Document late = slow;
        ClipById(late, kTalk).source_in = {980, 25};
        Check(
            !BuildResolveTimelineExport(late, exported, error) &&
                error.find("past the media") != std::string::npos,
            "un ralenti posé au-delà de la fin du rush est refusé : " + error);

        // Un arrêt sur image : Resolve fige la DERNIÈRE image de la plage.
        Document held = Montage();
        DocumentClip& frozen = ClipById(held, kTalk);
        frozen.source_in = {120, 25};
        frozen.duration = {25, 25};
        frozen.speed = {0, 1};
        Check(BuildResolveTimelineExport(held, exported, error),
              "un arrêt sur image se décrit : " + error);
        sent = SentClip(exported, kTalk);
        Check(sent != nullptr && sent->start_frame == 96 &&
                  sent->end_frame == 121 && sent->speed.num == 0,
              "l'image figée est la dernière de la plage posée");
        Check(SerializeResolveTimelineExport(exported).find(
                  "\"speed_percent\":0") != std::string::npos,
              "un arrêt sur image sort à 0 %");
        ClipById(held, kTalk).source_in = {10, 25};
        Check(!BuildResolveTimelineExport(held, exported, error) &&
                  error.find("Freeze Frame") != std::string::npos,
              "une image figée trop près du début du rush est refusée : " +
                  error);

        // Une rampe n'a pas d'API : sa vitesse moyenne lit la même source.
        Document ramp = Montage();
        DocumentClip& ramped = ClipById(ramp, kTalk);
        ramped.duration = {50, 25};
        ClipKeyframe from;
        from.id = "01K30000000000000000000R01";
        from.time = {0, 25};
        from.value = EffectParamValue{1, 1};
        from.interpolation = KeyframeInterpolation::Hold;
        ClipKeyframe to;
        to.id = "01K30000000000000000000R02";
        to.time = {25, 25};
        to.value = EffectParamValue{2, 1};
        to.interpolation = KeyframeInterpolation::Hold;
        ramped.speed_ramp = {from, to};
        Check(BuildResolveTimelineExport(ramp, exported, error),
              "une rampe se décrit : " + error);
        sent = SentClip(exported, kTalk);
        const ResolveSendLoss* loss = FindLoss(exported, "speed_ramps");
        Check(sent != nullptr && Same(sent->speed, 3, 2) && loss != nullptr &&
                  loss->ids == std::vector<Ulid>{kTalk},
              "une rampe part à sa vitesse moyenne, et c'est dit");
    }

    // --- Ce que l'API pose plan par plan -----------------------------------
    {
        Document styled = Montage();
        DocumentClip& talk = ClipById(styled, kTalk);
        talk.enabled = false;
        talk.opacity = {1, 2};
        talk.framing.mode = FramingMode::Fill;
        talk.framing.position_x = {1, 10};
        talk.framing.position_y = {1, 20};
        DocumentClip& voice = ClipById(styled, kVoice);
        voice.audio_gain_db = {-13, 2};
        voice.audio_fade_in = {5, 25};
        voice.audio_fade_out = {1, 50};
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(styled, exported, error),
              "un plan désactivé, translucide, cadré se décrit : " + error);
        const ResolveTimelineClip* sent = SentClip(exported, kTalk);
        Check(sent != nullptr && !sent->enabled && sent->opacity.num == 1 &&
                  sent->opacity.den == 2,
              "l'activation et l'opacité sont transmises");
        // Remplie : zoom 1 en « fill », 256/81 en « fit ». La position suit
        // la loi mesurée : Pan = x W W / dw0, Tilt = -y H H / dh0.
        Check(sent != nullptr && Same(sent->fill.zoom, 1, 1) &&
                  Same(sent->fit.zoom, 256, 81) &&
                  Same(sent->fill.pan, 2187, 32) &&
                  Same(sent->fill.tilt, -192, 1) && Same(sent->fit.pan, 216, 1),
              "le cadrage suit la loi de Resolve dans les deux réglages");
        const ResolveTimelineClip* sound = SentClip(exported, kVoice);
        Check(sound != nullptr && sound->gain_db.num == -13 &&
                  sound->gain_db.den == 2 && sound->fade_in == 5 &&
                  sound->fade_out == 1,
              "le gain et les fondus du son sont transmis, un fondu d'une "
              "demi-image arrondi à l'image");
        const ResolveSendLoss* rounded = FindLoss(exported, "fades_rounded");
        Check(rounded != nullptr && rounded->ids == std::vector<Ulid>{kVoice},
              "un fondu arrondi est nommé");
        const std::string json = SerializeResolveTimelineExport(exported);
        Check(json.find("\"enabled\":false") != std::string::npos &&
                  json.find("\"opacity\":50") != std::string::npos &&
                  json.find("\"volume_db\":-6.5") != std::string::npos &&
                  json.find("\"pan\":68.34375") != std::string::npos,
              "le descripteur porte activation, opacité, gain, cadrage : " +
                  json);

        // Ce que l'API ne porte pas est compté, pas refusé.
        Document lossy = Montage();
        DocumentClip& animated = ClipById(lossy, kTalk);
        animated.animation = ZoomTrack();
        animated.framing.crop_left = {1, 10};
        animated.blur = {10, 1};
        ClipById(lossy, kCut).mask.shape = kClipMaskEllipse;
        ClipEffect exposure;
        exposure.id = "01K30000000000000000000E01";
        exposure.type = "color.exposure";
        exposure.params = {{"amount", {3, 10}}};
        ClipById(lossy, kCut).effects = {exposure};
        Check(BuildResolveTimelineExport(lossy, exported, error),
              "un plan animé, recadré, flou, masqué, étalonné se décrit : " +
                  error);
        for (const char* what :
             {"animations", "crops", "blurs", "masks", "grades"})
            Check(FindLoss(exported, what) != nullptr,
                  std::string("la perte « ") + what + " » est nommée");
        const ResolveSendLoss* grades = FindLoss(exported, "grades");
        Check(grades != nullptr && grades->ids == std::vector<Ulid>{kCut},
              "chaque perte nomme ses plans");

        // L'étalonnage voyage sur demande, en CDL.
        ResolveSendOptions options;
        options.grade_cdl = true;
        options.cdl_node = 2;
        Check(BuildResolveTimelineExport(lossy, exported, error, options),
              "avec grade cdl : " + error);
        sent = SentClip(exported, kCut);
        Check(sent != nullptr && sent->graded &&
                  FindLoss(exported, "grades") == nullptr,
              "une exposition part en CDL");
        const std::string graded = SerializeResolveTimelineExport(exported);
        Check(graded.find("\"NodeIndex\":\"2\"") != std::string::npos &&
                  graded.find("\"grade\":\"cdl\"") != std::string::npos,
              "la CDL vise le nœud demandé : " + graded);
    }

    // --- Transitions ------------------------------------------------------
    {
        Document joined = Montage();
        // Deux plans bord à bord sur V1 : le plan parlant puis le plan de
        // coupe, avec des poignées des deux côtés, rangés dans l'ordre du
        // montage comme le document valide les range.
        ClipById(joined, kCut).source_in = {300, 25};
        std::swap(joined.sequence.tracks[0].clips[0],
                  joined.sequence.tracks[0].clips[1]);
        joined.sequence.transitions = {
            {"01K30000000000000000000T01",
             "01K30000000000000000000010",
             kTalk,
             kCut,
             "cross_dissolve",
             {10, 25},
             TransitionAlignment::Center},
            {"01K30000000000000000000T02",
             "01K30000000000000000000010",
             "",
             kTalk,
             "dip_to_color",
             {12, 25},
             TransitionAlignment::StartAtCut},
        };
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(joined, exported, error),
              "un fondu enchaîné et une ouverture au noir se décrivent : " +
                  error);
        Check(exported.transitions.size() == 1,
              "le fondu enchaîné part par AddTransition, l'ouverture non");
        if (exported.transitions.size() == 1) {
            const ResolveSendTransition& dissolve = exported.transitions[0];
            Check(dissolve.clip == 0 && dissolve.alignment == "center" &&
                      dissolve.type == "Cross Dissolve" &&
                      dissolve.category == "simple" &&
                      dissolve.duration == 10 && dissolve.record_start == 95 &&
                      dissolve.record_end == 105,
                  "le fondu s'accroche à la fin du plan sortant, centré");
        }
        const ResolveTimelineClip* talk = SentClip(exported, kTalk);
        Check(talk != nullptr && talk->fade_in == 12 && talk->fade_out == 0,
              "l'ouverture au noir devient un fondu d'opacité du premier "
              "plan, sans poignée");

        joined.sequence.transitions[0].alignment =
            TransitionAlignment::StartAtCut;
        joined.sequence.transitions[1].color = "#ff0000";
        Check(BuildResolveTimelineExport(joined, exported, error) &&
                  exported.transitions.size() == 1 &&
                  exported.transitions[0].alignment == "right" &&
                  exported.transitions[0].record_start == 100 &&
                  exported.transitions[0].record_end == 110,
              "un fondu qui commence sur la coupe s'aligne à droite");
        Check(FindLoss(exported, "dip_colors") != nullptr,
              "un fondu par une autre couleur que le noir est nommé");
    }

    // --- Marqueurs, sous-titres, look -------------------------------------
    {
        Document noted = Montage();
        DocumentMarker first;
        first.id = "01K30000000000000000000M01";
        first.name = "reprendre";
        first.time = {10, 25};
        first.color = "#4da3ff";
        DocumentMarker second;
        second.id = "01K30000000000000000000M02";
        second.name = "son";
        second.time = {10, 25};
        second.color = "#4da3ff";
        DocumentMarker third;
        third.id = "01K30000000000000000000M03";
        third.name = "plan";
        third.time = {50, 25};
        third.duration = {25, 25};
        third.color = "#123456";
        noted.sequence.markers = {third, first, second};
        DocumentTrack captions;
        captions.id = "01K30000000000000000000013";
        captions.kind = "caption";
        captions.index = 3;
        DocumentClip carton;
        carton.id = "01K30000000000000000000C01";
        carton.timeline_in = {0, 25};
        carton.duration = {50, 25};
        carton.caption_text = "bonjour";
        DocumentClip late = carton;
        late.id = "01K30000000000000000000C02";
        late.timeline_in = {120, 25};
        late.duration = {30, 25};
        late.caption_text = "à bientôt";
        captions.clips = {late, carton};
        DocumentTrack translated = captions;
        translated.id = "01K30000000000000000000014";
        translated.index = 4;
        for (DocumentClip& clip : translated.clips) clip.id[25] = 'X';
        noted.sequence.tracks.push_back(captions);
        noted.sequence.tracks.push_back(translated);
        noted.sequence.look_lut.digest = "abc";
        noted.sequence.look_lut.name = "look.cube";
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(noted, exported, error),
              "marqueurs, cartons et look se décrivent : " + error);
        Check(exported.markers.size() == 2,
              "deux marqueurs sur une même image n'en font qu'un");
        if (exported.markers.size() == 2) {
            Check(exported.markers[0].frame == 10 &&
                      exported.markers[0].name == "reprendre / son" &&
                      exported.markers[0].color == "Blue" &&
                      exported.markers[0].duration == 1 &&
                      exported.markers[0].marker_ids.size() == 2,
                  "le marqueur fusionné porte les deux noms, une image au "
                  "moins");
            Check(exported.markers[1].frame == 50 &&
                      exported.markers[1].duration == 25,
                  "un marqueur garde sa longueur");
        }
        Check(FindLoss(exported, "markers_merged") != nullptr &&
                  FindLoss(exported, "marker_colors") != nullptr,
              "la fusion et la couleur approchée sont nommées");
        Check(exported.subtitle_cues == 2 &&
                  exported.subtitles.find("00:00:04,800 --> 00:00:06,000\n"
                                          "à bientôt") != std::string::npos &&
                  exported.subtitles.rfind("1\n00:00:00,000", 0) == 0,
              "les cartons de la première piste partent en SRT, dans "
              "l'ordre : " +
                  exported.subtitles);
        Check(exported.duration == 150,
              "la longueur attendue compte le dernier carton");
        const ResolveSendLoss* tracks = FindLoss(exported, "caption_tracks");
        Check(tracks != nullptr &&
                  tracks->ids == std::vector<Ulid>{translated.id},
              "une seconde piste de sous-titres est nommée, pas mêlée");
        Check(FindLoss(exported, "caption_style") != nullptr &&
                  FindLoss(exported, "look_lut") != nullptr,
              "le style des cartons et le look sont nommés");
    }

    // --- Musique, solo -------------------------------------------------------
    {
        Document scored = Montage();
        LibraryMedia music = Media(kMusic, "music.wav", 25);
        music.has_video = false;
        music.rate = {0, 1};
        music.duration = {30, 1};
        scored.library.push_back(music);
        DocumentTrack bed;
        bed.id = "01K30000000000000000000015";
        bed.kind = "audio";
        bed.index = 5;
        bed.solo = true;
        bed.clips = {Clip("01K30000000000000000000024", kMusic, {0, 25}, {2, 1},
                          {125, 25})};
        scored.sequence.tracks.push_back(bed);
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(scored, exported, error),
              "un lit de musique se décrit : " + error);
        const ResolveTimelineClip* sent =
            SentClip(exported, "01K30000000000000000000024");
        Check(sent != nullptr && sent->audio && sent->track == 2 &&
                  sent->start_frame == 50 && sent->end_frame == 175,
              "un fichier son se compte en images de séquence (Resolve lui "
              "donne la cadence du projet)");
        Check(exported.tracks.size() == 3 && !exported.tracks[1].enabled &&
                  exported.tracks[2].enabled && !exported.tracks[2].mono,
              "la musique reste stéréo, et une piste en solo coupe les "
              "autres");
    }

    // --- Refus ------------------------------------------------------------
    {
        ResolveTimelineExport rejected;
        Document offGrid = Montage();
        ClipById(offGrid, kCut).timeline_in = {1, 50};
        Check(!BuildResolveTimelineExport(offGrid, rejected, error) &&
                  error.find("whole sequence frames") != std::string::npos,
              "une position entre deux images de séquence est refusée : " +
                  error);

        Document unknown = Montage();
        unknown.library.clear();
        Check(!BuildResolveTimelineExport(unknown, rejected, error) &&
                  error.find("not in the library") != std::string::npos,
              "un média absent de la médiathèque est refusé : " + error);

        // Une source à 3 i/s : un plan commencé à 1/25 s ne commence pas sur
        // une image source.
        Document halfFrame = Montage();
        halfFrame.library = {Media(kA, "C.MP4", 3), Media(kB, "B.MP4", 25)};
        ClipById(halfFrame, kTalk).source_in = {1, 25};
        ClipById(halfFrame, kTalk).duration = {25, 25};
        ClipById(halfFrame, kVoice).duration = {25, 25};
        Check(!BuildResolveTimelineExport(halfFrame, rejected, error) &&
                  error.find("whole frame of its source") != std::string::npos,
              "une coupe qui tombe entre deux images source est refusée : " +
                  error);

        Document silent;
        silent.sequence.frame_rate = {25, 1};
        Check(!BuildResolveTimelineExport(silent, rejected, error),
              "une timeline sans plan n'a rien à envoyer");
    }

    // --- Aucune suite n'envoie au Resolve ouvert sur la machine ------------
    // tests/test_environment.cc nomme un pont qui n'existe pas : une suite qui
    // appelle tous les outils l'a lancé pour de vrai le 13 novembre 2026.
    Check(!LocateResolveBridge().refusal.empty(),
          "aucune suite ne trouve le vrai pont Resolve");

    // --- L'envoi : le descripteur écrit, le pont lancé, son rapport relu ----
    // Un faux pont tient la place de sidecar/resolve_bridge.py : il relit le
    // descripteur et répond comme le vrai, sur la sortie standard.
    if (std::filesystem::exists("/usr/bin/python3")) {
        namespace fs = std::filesystem;
        const fs::path scratch =
            fs::temp_directory_path() /
            ("cutmachine_resolve_send_" + std::to_string(::getpid()));
        fs::remove_all(scratch);
        fs::create_directories(scratch);
        const fs::path bridge = scratch / "bridge.py";
        std::ofstream(bridge) << R"(import json, sys, time
args = sys.argv[1:]
mode = open(sys.argv[0] + ".mode").read().strip()
descriptor = json.load(open(args[args.index("--send") + 1]))
name = args[args.index("--name") + 1] if "--name" in args else descriptor["name"]
if mode == "refuse":
    print("Rushes absents du Media Pool : A.MP4.", file=sys.stderr)
    sys.exit(1)
if mode == "hang":
    time.sleep(30)
print("une phrase pour qui lit", file=sys.stderr)
kinds = [c["kind"] for c in descriptor["clips"]]
print(json.dumps({"ok": True, "timeline": name, "project": "projet",
                  "clips": len(kinds), "video_clips": kinds.count("video"),
                  "audio_clips": kinds.count("audio"), "verified": True,
                  "mismatches": []}))
)";
        const auto mode = [&](const char* value) {
            std::ofstream(bridge.string() + ".mode") << value;
        };
        ResolveBridgeSettings settings;
        settings.python = "/usr/bin/python3";
        settings.bridge = bridge.string();
        ResolveSendRequest request;
        request.directory = (scratch / "sent").string();
        request.name = "MONTAGE v2";
        Document lossy = Montage();
        ClipById(lossy, kTalk).blur = {4, 1};
        std::string result;
        std::string errorName;

        mode("ok");
        Check(SendTimelineToResolve(lossy, "/projet", request, settings, result,
                                    errorName, error),
              "un envoi qui aboutit : " + errorName + " " + error);
        mcp_json::Value answer;
        std::string parseError;
        Check(
            mcp_json::Value::Parse(result, answer, parseError) &&
                answer.Find("timeline") != nullptr &&
                answer.Find("timeline")->AsString() == "MONTAGE v2" &&
                answer.Find("verified") != nullptr &&
                answer.Find("verified")->AsBool() &&
                answer.Find("clips") != nullptr &&
                answer.Find("clips")->Dump() == "3",
            "la réponse porte le rapport du pont, le nom demandé : " + result);
        Check(result.find("\"not_sent\"") != std::string::npos &&
                  result.find("\"blurs\"") != std::string::npos &&
                  result.find("une phrase pour qui lit") != std::string::npos,
              "la réponse nomme ce qui n'est pas parti, et la phrase du "
              "pont : " +
                  result);
        const mcp_json::Value* written = answer.Find("descriptor");
        Check(written != nullptr && fs::exists(written->AsString()) &&
                  written->AsString().find("MONTAGE_v2-") != std::string::npos,
              "le descripteur reste lisible, nommé d'après la timeline");

        // Ce que la fenêtre en dit.
        const ResolveSendNotice told =
            ResolveSendNoticeFor(true, result, "", "");
        Check(!told.failed &&
                  told.status.rfind("Envoyé dans Resolve : « MONTAGE v2 », "
                                    "2 plans image, 1 plan son, en ",
                                    0) == 0 &&
                  told.status.find(" s, relu sans écart") != std::string::npos,
              "la ligne d'état dit où, combien et en combien de temps : " +
                  told.status);
        Check(told.title == "Envoyé dans Resolve, avec des manques" &&
                  told.text.find("• flous : 1") != std::string::npos,
              "ce qui n'est pas parti s'affiche en français : " + told.text);
        Check(result.find("\"elapsed_ms\":") != std::string::npos,
              "la réponse porte la durée de l'envoi");
        const ResolveSendNotice clean = ResolveSendNoticeFor(
            true,
            R"({"timeline":"T","verified":true,"video_clips":1,)"
            R"("audio_clips":0,"elapsed_ms":4500,"not_sent":[],)"
            R"("mismatches":[]})",
            "", "");
        Check(clean.title.empty() &&
                  clean.status ==
                      "Envoyé dans Resolve : « T », 1 plan image, "
                      "0 plan son, en 4,5 s, relu sans écart",
              "un envoi sans manque ne tient que dans la ligne d'état : " +
                  clean.status);
        const ResolveSendNotice off =
            ResolveSendNoticeFor(false, "", "ResolveTimeout", "after 600 s");
        Check(off.failed && off.text.find("fenêtre") != std::string::npos,
              "un Resolve muet est dit en français");

        mode("refuse");
        Check(!SendTimelineToResolve(lossy, "/projet", request, settings,
                                     result, errorName, error) &&
                  errorName == "ResolveSendFailed" &&
                  error == "Rushes absents du Media Pool : A.MP4.",
              "le refus du pont remonte avec sa phrase : " + error);
        Check(ResolveSendNoticeFor(false, "", errorName, error).text == error,
              "la fenêtre montre la phrase du pont telle quelle");

        mode("hang");
        request.timeout_seconds = 1;
        Check(!SendTimelineToResolve(lossy, "/projet", request, settings,
                                     result, errorName, error) &&
                  errorName == "ResolveTimeout",
              "un Resolve qui ne répond plus est abandonné : " + errorName);

        request.dry_run = true;
        settings.refusal = "Resolve bridge not found";
        Check(SendTimelineToResolve(lossy, "/projet", request, settings, result,
                                    errorName, error) &&
                  result.find("\"dry_run\":true") != std::string::npos &&
                  result.find("\"video_clips\":2") != std::string::npos,
              "dry_run écrit le descripteur sans chercher le pont : " + result);
        request.dry_run = false;
        Check(!SendTimelineToResolve(lossy, "/projet", request, settings,
                                     result, errorName, error) &&
                  errorName == "ResolveBridgeMissing",
              "sans pont, l'envoi le dit");
        fs::remove_all(scratch);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All resolve export tests passed\n";
    return 0;
}
