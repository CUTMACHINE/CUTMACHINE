#include "AggregateMembers.h"
#include "Document.h"
#include "DocumentDelta.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "TestSupport.h"

namespace {

DocumentClip Clip(const std::string& id, int64_t timelineIn, int64_t duration) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = "01K30000000000000000000001";
    clip.source_in = RationalTime{0, 25};
    clip.duration = RationalTime{duration, 25};
    clip.timeline_in = RationalTime{timelineIn, 25};
    clip.include_audio = false;
    return clip;
}

// Trois plans bout à bout sur une piste vidéo : la forme d'un montage.
Document Base() {
    Document document;
    document.sequence.id = "01K300000000000000000000SQ";
    document.sequence.name = "MONTAGE";
    document.sequence.frame_rate = {25, 1};
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    DocumentTrack video;
    video.id = "01K30000000000000000000010";
    video.kind = "video";
    video.index = 0;
    video.clips = {Clip("01K3000000000000000000000A", 0, 100),
                   Clip("01K3000000000000000000000B", 100, 100),
                   Clip("01K3000000000000000000000C", 200, 100)};
    document.sequence.tracks = {video};
    return document;
}

DocumentTrack& Video(Document& document) { return document.sequence.tracks[0]; }

// Where every clip sits, compared by value: what a caller's own model of the
// timeline holds.
struct Place {
    Ulid track_id;
    RationalTime source_in{0, 1};
    RationalTime duration{0, 1};
    RationalTime timeline_in{0, 1};
    bool operator==(const Place& other) const {
        return track_id == other.track_id &&
               source_in.compare(other.source_in) == 0 &&
               duration.compare(other.duration) == 0 &&
               timeline_in.compare(other.timeline_in) == 0;
    }
};
using Places = std::map<Ulid, Place>;

Places Positions(const Document& document) {
    Places places;
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            places[clip.id] = {track.id, clip.source_in, clip.duration,
                               clip.timeline_in};
    return places;
}

// The delta applied literally to the state before, the way the MCP server's
// instructions tell a caller to: rules move the clips they cover, `clips`
// replaces what it names, removed ids go. A clip both covered by a rule and
// listed would make the order of those steps matter, so it fails the check.
Places ApplyDelta(const Document& before, const DocumentDelta& delta) {
    Places places;
    std::set<Ulid> listed;
    for (const DeltaClip& clip : delta.clips) listed.insert(clip.clip_id);
    for (const DocumentTrack& track : before.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (std::count(delta.removed_clip_ids.begin(),
                           delta.removed_clip_ids.end(), clip.id) != 0)
                continue;
            Place place{track.id, clip.source_in, clip.duration,
                        clip.timeline_in};
            for (const DeltaShift& shift : delta.shifted) {
                if (shift.track_id != track.id ||
                    clip.timeline_in.compare(shift.from) < 0 ||
                    (shift.until &&
                     clip.timeline_in.compare(*shift.until) >= 0))
                    continue;
                if (listed.count(clip.id) != 0) return {};
                place.timeline_in = place.timeline_in.add(shift.by);
            }
            places[clip.id] = place;
        }
    }
    for (const DeltaClip& clip : delta.clips)
        places[clip.clip_id] = {clip.track_id, clip.source_in, clip.duration,
                                clip.timeline_in};
    return places;
}

}  // namespace

int main() {
    // --- ClipsEqual : une garde par champ ---------------------------------
    // Si DocumentClip gagne un champ et que ClipsEqual ne le compare pas, le
    // delta sous-déclare en silence. Chaque champ a son cas ici.
    {
        const DocumentClip base = Clip("01K3000000000000000000000A", 0, 100);
        Check(ClipsEqual(base, base), "un clip est égal à lui-même");
        struct Case {
            const char* name;
            DocumentClip mutated;
        };
        std::vector<Case> cases;
        auto with = [&](const char* name, void (*apply)(DocumentClip&)) {
            DocumentClip copy = base;
            apply(copy);
            cases.push_back({name, copy});
        };
        with("id",
             [](DocumentClip& c) { c.id = "01K3000000000000000000000Z"; });
        with("source_id", [](DocumentClip& c) {
            c.source_id = "01K30000000000000000000009";
        });
        with("source_in",
             [](DocumentClip& c) { c.source_in = RationalTime{5, 25}; });
        with("duration",
             [](DocumentClip& c) { c.duration = RationalTime{50, 25}; });
        with("timeline_in",
             [](DocumentClip& c) { c.timeline_in = RationalTime{7, 25}; });
        with("include_audio",
             [](DocumentClip& c) { c.include_audio = !c.include_audio; });
        with("link_group_id", [](DocumentClip& c) {
            c.link_group_id = "01K3000000000000000000000L";
        });
        with("sync_anchor_clip_id", [](DocumentClip& c) {
            c.sync_anchor_clip_id = "01K3000000000000000000000S";
        });
        with("sync_reference_delta", [](DocumentClip& c) {
            c.sync_reference_delta = RationalTime{3, 25};
        });
        with("effects", [](DocumentClip& c) {
            ClipEffect effect;
            effect.id = "01K3000000000000000000000E";
            effect.type = "color.exposure";
            effect.params["value"] = EffectParamValue{1, 2};
            c.effects.push_back(effect);
        });
        with("caption_group_id", [](DocumentClip& c) {
            c.caption_group_id = "01K3000000000000000000000G";
        });
        with("caption_text",
             [](DocumentClip& c) { c.caption_text = "bonjour"; });
        with("opacity",
             [](DocumentClip& c) { c.opacity = EffectParamValue{1, 2}; });
        with("audio_gain_db", [](DocumentClip& c) {
            c.audio_gain_db = EffectParamValue{-6, 1};
        });
        with("audio_fade_in",
             [](DocumentClip& c) { c.audio_fade_in = RationalTime{12, 25}; });
        with("audio_fade_out",
             [](DocumentClip& c) { c.audio_fade_out = RationalTime{12, 25}; });
        // MOTION-2026-09 M4.
        with("composition_id", [](DocumentClip& c) {
            c.composition_id = "01K3000000000000000000000C";
        });
        with("motion_props", [](DocumentClip& c) {
            c.motion_props = mcp_json::Value::MakeObject();
            c.motion_props.Set("titre", mcp_json::Value::MakeString("Bonjour"));
        });
        // R3 -- docs/roadmap/REEL-2026-09.md.
        with("speed",
             [](DocumentClip& c) { c.speed = EffectParamValue{1, 2}; });
        // R1 -- docs/roadmap/REEL-2026-09.md.
        with("framing", [](DocumentClip& c) {
            c.framing.mode = FramingMode::Fill;
            c.framing.scale = EffectParamValue{11, 10};
        });
        // R2 -- docs/roadmap/REEL-2026-09.md.
        with("animation", [](DocumentClip& c) {
            ClipParameterAnimation track;
            track.parameter = ClipAnimatedParameter::FramingScale;
            ClipKeyframe first;
            first.id = "01K80000000000000000000K01";
            first.time = {0, 25};
            first.value = EffectParamValue{1, 1};
            ClipKeyframe second = first;
            second.id = "01K80000000000000000000K02";
            second.time = {12, 25};
            second.value = EffectParamValue{23, 20};
            track.keyframes = {first, second};
            c.animation = {track};
        });
        // R3, critère 4 -- une rampe remodelée est un plan qui a changé.
        with("speed_ramp", [](DocumentClip& c) {
            ClipKeyframe first;
            first.id = "01K80000000000000000000R01";
            first.time = {0, 25};
            first.value = EffectParamValue{1, 1};
            ClipKeyframe second = first;
            second.id = "01K80000000000000000000R02";
            second.time = {10, 25};
            second.value = EffectParamValue{1, 4};
            c.speed_ramp = {first, second};
        });
        // Hors lot -- l'auteur, 29 octobre 2026 : un plan désactivé a changé.
        with("enabled", [](DocumentClip& c) { c.enabled = false; });
        // EF3 -- docs/roadmap/EFFETS-2026-10.md.
        with("blur", [](DocumentClip& c) { c.blur = EffectParamValue{10, 1}; });
        // PA4 -- docs/roadmap/PARITE-2026-11.md.
        with("caption_word_times",
             [](DocumentClip& c) { c.caption_word_times = {{1, 2}}; });
        // PA8 -- docs/roadmap/PARITE-2026-11.md.
        with("mask", [](DocumentClip& c) { c.mask.shape = "ellipse"; });
        for (const Case& item : cases)
            Check(!ClipsEqual(base, item.mutated),
                  std::string("un changement de ") + item.name +
                      " rend les clips différents");
        // Compté sur la struct elle-même, pas recopié : un champ ajouté sans
        // son cas ici fait échouer ce test au lieu de passer inaperçu.
        Check(cases.size() == AggregateMemberCount<DocumentClip>::value,
              "chaque champ de DocumentClip a son cas (" +
                  std::to_string(cases.size()) + " cas)");
        // Un effet identique en valeur mais rangé sous une autre clé compte.
        DocumentClip renamed = base;
        ClipEffect effect;
        effect.id = "01K3000000000000000000000E";
        effect.type = "color.exposure";
        effect.params["autre"] = EffectParamValue{1, 2};
        renamed.effects.push_back(effect);
        Check(!ClipsEqual(base, cases.back().mutated) &&
                  !ClipsEqual(renamed, cases[9].mutated),
              "un paramètre d'effet renommé n'est pas la même chose");
    }

    // --- Rien n'a bougé ----------------------------------------------------
    {
        const Document document = Base();
        DocumentDelta delta;
        Check(!ComputeDocumentDelta(document, document, delta),
              "un document identique ne déclare aucun changement");
        Check(delta.clips.empty() && delta.shifted.empty() &&
                  delta.removed_clip_ids.empty(),
              "et le delta est vide");
        Check(delta.duration == RationalTime{300, 25},
              "mais la durée est publiée quand même");
    }

    // --- Un ripple devient une règle, pas une liste -------------------------
    {
        const Document before = Base();
        Document after = before;
        // La tête du premier plan est rognée de 20 images : les deux suivants
        // remontent d'autant. C'est le geste le plus fréquent du montage.
        Video(after).clips[0].source_in = RationalTime{20, 25};
        Video(after).clips[0].duration = RationalTime{80, 25};
        Video(after).clips[1].timeline_in = RationalTime{80, 25};
        Video(after).clips[2].timeline_in = RationalTime{180, 25};
        DocumentDelta delta;
        Check(ComputeDocumentDelta(before, after, delta), "le ripple change");
        Check(delta.clips.size() == 1 &&
                  delta.clips[0].clip_id == "01K3000000000000000000000A" &&
                  delta.clips[0].duration == RationalTime{80, 25},
              "le clip rogné sort en état résultant");
        Check(delta.shifted.size() == 1 && delta.shifted[0].count == 2 &&
                  delta.shifted[0].by == RationalTime{-20, 25} &&
                  delta.shifted[0].from == RationalTime{100, 25},
              "les deux plans déplacés tiennent en une règle signée");
        Check(delta.duration == RationalTime{280, 25},
              "la nouvelle durée est publiée");
    }

    // --- Le son d'un plan est un changement --------------------------------
    {
        // QC-2026-09 A7 : gain et fondus font partie du montage. Un agent qui
        // les règle doit l'apprendre du delta, pas en relisant le document.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].audio_gain_db = EffectParamValue{-6, 1};
        Video(after).clips[2].audio_fade_out = RationalTime{12, 25};
        DocumentDelta delta;
        Check(ComputeDocumentDelta(before, after, delta),
              "un réglage de gain ou de fondu change le document");
        Check(delta.clips.size() == 2 && delta.shifted.empty(),
              "les deux plans réglés sortent en clips individuels");
    }
    {
        // Déplacés ensemble mais l'un d'eux a changé de gain : ce n'est plus
        // un simple décalage, et le replier dans une règle tairait le gain.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{120, 25};
        Video(after).clips[2].timeline_in = RationalTime{220, 25};
        Video(after).clips[2].audio_gain_db = EffectParamValue{3, 1};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        bool gainReported = false;
        for (const DeltaClip& clip : delta.clips)
            gainReported |= clip.clip_id == "01K3000000000000000000000C";
        Check(gainReported,
              "un plan déplacé dont le gain change n'est pas replié en "
              "décalage");
    }

    // --- Une règle ne s'invente pas ----------------------------------------
    {
        // Un seul plan bouge, alors qu'un autre le suit sans bouger : aucune
        // règle ne peut décrire ça sans mentir sur le plan resté en place.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{120, 25};
        DocumentDelta delta;
        Check(ComputeDocumentDelta(before, after, delta),
              "le déplacement change");
        Check(delta.shifted.empty(),
              "un déplacement partiel ne devient pas une règle");
        Check(delta.clips.size() == 1 &&
                  delta.clips[0].timeline_in == RationalTime{120, 25},
              "il sort en clip individuel");
    }
    {
        // Deux plans bougent, de valeurs différentes : pas une règle non plus.
        const Document before = Base();
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{110, 25};
        Video(after).clips[2].timeline_in = RationalTime{230, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.shifted.empty() && delta.clips.size() == 2,
              "deux décalages inégaux restent deux clips");
    }

    // --- Un ripple par tronçon (AX-2026-09) ---------------------------------
    {
        // Deux mots retirés dans un même montage : la piste remonte de 10
        // images après la première coupe, de 15 après la seconde. Une seule
        // règle par piste ne pouvait décrire ni l'un ni l'autre, et le delta
        // listait alors chaque plan.
        Document before = Base();
        Video(before).clips = {Clip("01K3000000000000000000000A", 0, 100),
                               Clip("01K3000000000000000000000B", 100, 100),
                               Clip("01K3000000000000000000000C", 200, 100),
                               Clip("01K3000000000000000000000D", 300, 100),
                               Clip("01K3000000000000000000000E", 400, 100),
                               Clip("01K3000000000000000000000F", 500, 100)};
        Document after = before;
        Video(after).clips[0].duration = RationalTime{90, 25};
        Video(after).clips[1].timeline_in = RationalTime{90, 25};
        Video(after).clips[2].timeline_in = RationalTime{190, 25};
        Video(after).clips[3].timeline_in = RationalTime{290, 25};
        Video(after).clips[3].duration = RationalTime{95, 25};
        Video(after).clips[4].timeline_in = RationalTime{385, 25};
        Video(after).clips[5].timeline_in = RationalTime{485, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.clips.size() == 2,
              "seuls les deux plans rognés sortent en état résultant");
        Check(delta.shifted.size() == 2, "deux tronçons, deux règles");
        if (delta.shifted.size() == 2) {
            const DeltaShift& first = delta.shifted[0];
            const DeltaShift& second = delta.shifted[1];
            Check(first.from == RationalTime{100, 25} &&
                      first.by == RationalTime{-10, 25} && first.count == 2 &&
                      first.until && *first.until == RationalTime{300, 25},
                  "le premier tronçon s'arrête au plan rogné suivant");
            Check(second.from == RationalTime{400, 25} &&
                      second.by == RationalTime{-15, 25} && second.count == 2 &&
                      !second.until,
                  "le dernier court jusqu'au bout de la piste");
        }
        Check(ApplyDelta(before, delta) == Positions(after),
              "le delta appliqué à la lettre redonne le montage");
        const std::string json = SerializeDocumentDelta(delta);
        Check(json.find("\"until\"") != std::string::npos,
              "la borne est publiée : " + json);
    }
    {
        // Un tronçon qui bouge, suivi d'un plan resté en place : la règle
        // s'arrête avant lui, elle ne le déplace pas.
        Document before = Base();
        Video(before).clips.push_back(
            Clip("01K3000000000000000000000D", 400, 50));
        Document after = before;
        Video(after).clips[1].timeline_in = RationalTime{110, 25};
        Video(after).clips[2].timeline_in = RationalTime{210, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.clips.empty() && delta.shifted.size() == 1 &&
                  delta.shifted[0].count == 2 && delta.shifted[0].until &&
                  *delta.shifted[0].until == RationalTime{400, 25},
              "une règle bornée par le plan immobile");
        Check(ApplyDelta(before, delta) == Positions(after),
              "et elle s'applique à la lettre");
    }
    {
        // Création, suppression et décalages mêlés sur deux pistes : ce
        // qu'un plan de montage produit. Le delta doit suffire à passer de
        // l'avant à l'après sans relire le document.
        Document before = Base();
        DocumentTrack audio = Video(before);
        audio.id = "01K30000000000000000000011";
        audio.kind = "audio";
        audio.index = 1;
        audio.clips[0].id = "01K300000000000000000000AA";
        audio.clips[1].id = "01K300000000000000000000AB";
        audio.clips[2].id = "01K300000000000000000000AC";
        audio.clips.push_back(Clip("01K300000000000000000000AD", 300, 100));
        audio.clips.push_back(Clip("01K300000000000000000000AE", 400, 100));
        before.sequence.tracks.push_back(audio);
        Document after = before;
        DocumentTrack& a = after.sequence.tracks[1];
        a.clips[0].duration = RationalTime{40, 25};
        a.clips.insert(a.clips.begin() + 1,
                       Clip("01K300000000000000000000AF", 40, 50));
        a.clips[2].timeline_in = RationalTime{90, 25};
        a.clips[3].timeline_in = RationalTime{190, 25};
        a.clips.erase(a.clips.begin() + 4);  // AD
        a.clips[4].timeline_in = RationalTime{290, 25};
        Video(after).clips[2].timeline_in = RationalTime{210, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(ApplyDelta(before, delta) == Positions(after),
              "création, suppression et tronçons s'appliquent à la lettre");
        Check(delta.removed_clip_ids.size() == 1, "le plan retiré est nommé");
    }

    // --- Création, suppression, pistes, séquence ---------------------------
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips.push_back(
            Clip("01K3000000000000000000000D", 300, 50));
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.clips.size() == 1 && delta.clips[0].created,
              "un clip ajouté est marqué créé");
    }
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips.erase(Video(after).clips.begin() + 1);
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.removed_clip_ids.size() == 1 &&
                  delta.removed_clip_ids[0] == "01K3000000000000000000000B",
              "un clip retiré sort par son identifiant");
    }
    {
        const Document before = Base();
        Document after = before;
        DocumentTrack overlay;
        overlay.id = "01K30000000000000000000011";
        overlay.kind = "video";
        overlay.index = 2;
        after.sequence.tracks.push_back(overlay);
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.created_tracks.size() == 1 &&
                  delta.created_tracks[0].index == 2 &&
                  delta.created_tracks[0].kind == "video",
              "une piste créée sort avec son genre et son index");
        DocumentDelta back;
        ComputeDocumentDelta(after, before, back);
        Check(back.removed_track_ids.size() == 1,
              "et sa suppression sort par identifiant");
    }
    {
        // ITW7 -- a created style, marker, transition or bin is addressable
        // from the result, and its removal reported by identity.
        const Document before = Base();
        Document after = before;
        CaptionStyle style;
        style.id = "01K30000000000000000000021";
        after.sequence.caption_styles.push_back(style);
        DocumentMarker marker;
        marker.id = "01K30000000000000000000022";
        after.sequence.markers.push_back(marker);
        DocumentBin bin;
        bin.id = "01K30000000000000000000023";
        bin.name = "Plans de coupe";
        after.bins.push_back(bin);
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        const std::string json = SerializeDocumentDelta(delta);
        Check(delta.caption_styles.created ==
                      std::vector<Ulid>{"01K30000000000000000000021"} &&
                  delta.markers.created ==
                      std::vector<Ulid>{"01K30000000000000000000022"} &&
                  delta.bins.created ==
                      std::vector<Ulid>{"01K30000000000000000000023"} &&
                  json.find("\"created_caption_style_ids\":[\"01K3000000000"
                            "0000000000021\"]") != std::string::npos &&
                  json.find("transition") == std::string::npos,
              "un style, un marqueur et un chutier créés sortent par "
              "identifiant : " +
                  json);
        DocumentDelta back;
        ComputeDocumentDelta(after, before, back);
        Check(back.caption_styles.removed.size() == 1 &&
                  back.markers.removed.size() == 1 &&
                  back.bins.removed.size() == 1 &&
                  back.caption_styles.created.empty(),
              "et leur suppression aussi");
    }
    {
        const Document before = Base();
        Document after = before;
        after.sequence.width = 2160;
        after.sequence.height = 3840;
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        Check(delta.sequence_changed && delta.clips.empty(),
              "un changement de format se signale sans lister les plans");
    }

    // --- Sérialisation -----------------------------------------------------
    {
        const Document before = Base();
        Document after = before;
        Video(after).clips[0].duration = RationalTime{80, 25};
        Video(after).clips[1].timeline_in = RationalTime{80, 25};
        Video(after).clips[2].timeline_in = RationalTime{180, 25};
        DocumentDelta delta;
        ComputeDocumentDelta(before, after, delta);
        const std::string json = SerializeDocumentDelta(delta);
        Check(json.find("\"shifted\"") != std::string::npos &&
                  json.find("\"duration\"") != std::string::npos,
              "le rapport porte la règle et la durée : " + json);
        Check(json.find("removed_clip_ids") == std::string::npos &&
                  json.find("created_tracks") == std::string::npos,
              "et tait les sections vides : " + json);
    }
    {
        const Document document = Base();
        DocumentDelta delta;
        ComputeDocumentDelta(document, document, delta);
        const std::string json = SerializeDocumentDelta(delta);
        Check(json.find("clips") == std::string::npos,
              "un delta vide ne fabrique pas un tableau de clips : " + json);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All document delta tests passed\n";
    return 0;
}
