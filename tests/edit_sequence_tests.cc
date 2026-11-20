// tests/edit_sequence_tests.cc -- stateful edit-log regression coverage.
//
// Tests de séquences d'opérations générées (stateful testing).
//
// Principe : générer des séquences aléatoires mais reproductibles
// d'opérations
// valides sur un Document, et vérifier des invariants à chaque étape :
//
//   I1  document.Validate() == true après chaque Apply réussi
//   I2  tous les IDs sont uniques dans le document
//   I3  aucune durée de clip n'est nulle ou négative
//   I4  Undo ramène exactement à l'état précédent (byte-identical)
//   I5  Redo ramène exactement à l'état post-Apply (byte-identical)
//   I6  la sérialisation est idempotente : Serialize(Deserialize(s)) == s
//   I7  N Apply puis N Undo ramène à l'état initial
//   I8  la pile d'opérations est cohérente avec AppliedCount/UndoneCount
//   I9  le journal relu annule et refait comme le journal en mémoire, une
//       opération ou un lot (PL1) à la fois
//
// Chaque sous-test utilise une graine fixe (LCG) pour être reproductible,
// mais différente entre sous-tests pour couvrir des territoires variés.

#include "Document.h"
#include "EditLog.h"
#include "Json.h"
#include "MotionDocument.h"
#include "MotionOperations.h"
#include "Operations.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "TestSupport.h"

namespace {

// -----------------------------------------------------------------------
// Infrastructure de test (même style que le reste de la suite)
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Mini LCG déterministe (pas de dépendance <random>)
// -----------------------------------------------------------------------
struct Lcg {
    explicit Lcg(uint64_t seed) : state_(seed) {}
    uint64_t Next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_;
    }
    int64_t NextInt64(int64_t lo, int64_t hi) {
        if (lo >= hi) return lo;
        return lo +
               static_cast<int64_t>(Next() % static_cast<uint64_t>(hi - lo));
    }
    bool NextBool() { return Next() % 2 == 0; }
    size_t NextIndex(size_t size) {
        if (size == 0) return 0;
        return static_cast<size_t>(Next() % size);
    }

private:
    uint64_t state_;
};

// -----------------------------------------------------------------------
// Invariants vérifiables sur Document
// -----------------------------------------------------------------------

bool AllIdsUnique(const Document& doc, std::string& reason) {
    std::vector<std::string> ids;
    for (const auto& track : doc.sequence.tracks) {
        ids.push_back(track.id);
        for (const auto& clip : track.clips) {
            ids.push_back(clip.id);
        }
    }
    for (const auto& marker : doc.sequence.markers) {
        ids.push_back(marker.id);
    }
    std::sort(ids.begin(), ids.end());
    const auto dup = std::adjacent_find(ids.begin(), ids.end());
    if (dup != ids.end()) {
        reason = "ID dupliqué : " + *dup;
        return false;
    }
    return true;
}

bool NoDegenerateClip(const Document& doc, std::string& reason) {
    for (const auto& track : doc.sequence.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.duration <= RationalTime{0, clip.duration.rate}) {
                reason = "clip " + clip.id + " durée <= 0";
                return false;
            }
        }
    }
    return true;
}

void CheckInvariants(const Document& doc, const std::string& context) {
    std::string error;
    Check(doc.Validate(error), context + " : Validate() : " + error);
    Check(AllIdsUnique(doc, error), context + " : IDs uniques : " + error);
    Check(NoDegenerateClip(doc, error),
          context + " : clips valides : " + error);
}

// -----------------------------------------------------------------------
// Générateur d'opérations valides
// Retourne {ok, operation} où ok == false si le document est trop vide
// pour que l'opération soit applicable.
// -----------------------------------------------------------------------

struct GeneratedOp {
    bool applicable = false;
    Operation op;
};

// Rates réalistes
const int32_t kRate = 25;  // frames @ 25fps pour toute la suite

Document MakeBaseDocument() {
    Document doc;
    doc.sequence.name = "test";
    doc.sequence.frame_rate = {25, 1};
    doc.sequence.width = 1920;
    doc.sequence.height = 1080;
    DocumentSource source;
    source.id = GenerateUlid();
    source.path = "/tmp/stateful-source.mov";
    source.rate = {25, 1};
    source.duration = {100000, 25};
    doc.sources.push_back(std::move(source));
    return doc;
}

// Ajouter une piste vidéo (toujours applicable)
GeneratedOp GenAddTrack(const Document& doc, Lcg& rng) {
    AddTrackOperation op;
    op.kind = rng.NextBool() ? "video" : "audio";
    op.index = 0;
    for (const DocumentTrack& track : doc.sequence.tracks) {
        op.index = std::max(op.index, track.index + 1);
    }
    op.track_id = GenerateUlid();
    return {true, op};
}

// Ajouter un clip sur une piste existante
GeneratedOp GenAddClip(const Document& doc, Lcg& rng) {
    if (doc.sequence.tracks.empty()) return {false, {}};
    const size_t ti = rng.NextIndex(doc.sequence.tracks.size());
    const auto& track = doc.sequence.tracks[ti];

    // Trouver un timeline_in qui ne chevauche pas les clips existants
    // Stratégie simple : mettre après le dernier clip de la piste
    int64_t latestEnd = 0;
    for (const auto& clip : track.clips) {
        const int64_t end = clip.timeline_in.add(clip.duration).value;
        if (end > latestEnd) latestEnd = end;
    }
    // Durée : 24-200 frames
    const int64_t dur = rng.NextInt64(24, 201);
    const int64_t gap = rng.NextInt64(0, 25);  // 0-24 frames de gap

    InsertClipOperation op;
    op.track_id = track.id;
    op.source_id = doc.sources.front().id;
    op.source_in = {0, kRate};
    op.timeline_in = {latestEnd + gap, kRate};
    op.duration = {dur, kRate};
    return {true, op};
}

// Ajouter un marqueur
GeneratedOp GenAddMarker(const Document& doc, Lcg& rng) {
    DocumentMarker marker;
    marker.id = GenerateUlid();
    marker.name = "M" + std::to_string(rng.Next() % 1000);
    marker.time = {rng.NextInt64(0, 3600), kRate};
    AddMarkerOperation op;
    op.marker = marker;
    return {true, op};
}

// Supprimer un clip
GeneratedOp GenRemoveClip(const Document& doc, Lcg& rng) {
    // Collecter tous les clips
    std::vector<std::string> clipIds;
    for (const auto& track : doc.sequence.tracks)
        for (const auto& clip : track.clips) clipIds.push_back(clip.id);
    if (clipIds.empty()) return {false, {}};
    const std::string id = clipIds[rng.NextIndex(clipIds.size())];
    RemoveClipOperation op;
    op.clip_id = id;
    return {true, op};
}

// Supprimer un marqueur
GeneratedOp GenRemoveMarker(const Document& doc, Lcg& rng) {
    if (doc.sequence.markers.empty()) return {false, {}};
    const size_t i = rng.NextIndex(doc.sequence.markers.size());
    RemoveMarkerOperation op;
    op.marker_id = doc.sequence.markers[i].id;
    return {true, op};
}

std::vector<const DocumentClip*> ClipsOnTracks(const Document& doc,
                                               const std::string& kind) {
    std::vector<const DocumentClip*> clips;
    for (const auto& track : doc.sequence.tracks) {
        if (!kind.empty() && track.kind != kind) continue;
        for (const auto& clip : track.clips) clips.push_back(&clip);
    }
    return clips;
}

// Régler gain et fondus d'un clip audio (QC-2026-09 A7)
GeneratedOp GenSetClipAudio(const Document& doc, Lcg& rng) {
    const auto clips = ClipsOnTracks(doc, "audio");
    if (clips.empty()) return {false, {}};
    const DocumentClip& clip = *clips[rng.NextIndex(clips.size())];
    SetClipAudioOperation op;
    op.clip_id = clip.id;
    op.gain_db = {static_cast<int32_t>(rng.NextInt64(-120, 49)), 1};
    op.fade_in = {rng.NextInt64(0, clip.duration.value + 1),
                  clip.duration.rate};
    op.fade_out = {rng.NextInt64(0, clip.duration.value + 1),
                   clip.duration.rate};
    return {true, op};
}

// Régler l'opacité d'un clip vidéo
GeneratedOp GenSetClipOpacity(const Document& doc, Lcg& rng) {
    const auto clips = ClipsOnTracks(doc, "video");
    if (clips.empty()) return {false, {}};
    SetClipOpacityOperation op;
    op.clip_id = clips[rng.NextIndex(clips.size())]->id;
    const int32_t den = static_cast<int32_t>(rng.NextInt64(1, 101));
    op.opacity = {static_cast<int32_t>(rng.NextInt64(0, den + 1)), den};
    return {true, op};
}

// Déplacer un clip après le dernier clip de sa piste. Enregistre l'état
// exact des pistes, comme toutes les éditions qui écrasent ou découpent.
GeneratedOp GenMoveClip(const Document& doc, Lcg& rng) {
    const auto clips = ClipsOnTracks(doc, "");
    if (clips.empty()) return {false, {}};
    const DocumentClip& clip = *clips[rng.NextIndex(clips.size())];
    const DocumentTrack* track = doc.FindTrackForClip(clip.id);
    int64_t latestEnd = 0;
    for (const auto& other : track->clips) {
        latestEnd =
            std::max(latestEnd, other.timeline_in.add(other.duration).value);
    }
    MoveClipOperation op;
    op.clip_id = clip.id;
    op.track_id = track->id;
    op.timeline_in = {latestEnd + rng.NextInt64(0, 25), kRate};
    return {true, op};
}

// Effacer un clip sans refermer le vide (état exact des pistes)
GeneratedOp GenClearClip(const Document& doc, Lcg& rng) {
    const auto clips = ClipsOnTracks(doc, "");
    if (clips.empty()) return {false, {}};
    ClearClipOperation op;
    op.clip_id = clips[rng.NextIndex(clips.size())]->id;
    return {true, op};
}

// -----------------------------------------------------------------------
// MOTION-2026-09 M3 -- compositions du projet, modifiées par les opérations
// motion, dans les mêmes séquences que le montage : les invariants I1 à I9
// portent aussi sur elles.
// -----------------------------------------------------------------------

mcp_json::Value ParseJson(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    if (!mcp_json::Value::Parse(text, value, error)) {
        throw std::runtime_error("JSON de test invalide : " + error);
    }
    return value;
}

std::string Decimal(Lcg& rng) {
    return std::to_string(rng.NextInt64(0, 10)) + "." +
           std::to_string(rng.NextInt64(0, 100));
}

GeneratedOp GenAddComposition(const Document&, Lcg& rng) {
    AddMotionCompositionOperation op;
    op.composition.id = GenerateUlid();
    op.composition.name = "Carton " + std::to_string(rng.Next() % 1000);
    op.composition.definition = ParseJson(
        R"({"width":1080,"height":1920,"fps":25,"duration":60,)"
        R"("props":{"titre":{"type":"text","default":"Titre"},)"
        R"("taille":{"type":"number","default":80,"min":10,"max":200},)"
        R"("position":{"type":"point"}},)"
        R"("root":{"type":"fill","style":{"justifyContent":"center"},)"
        R"("children":[{"type":"box","id":"bloc","style":{"padding":12},)"
        R"("children":[{"type":"text","content":{"prop":"titre"},)"
        R"("style":{"fontSize":{"prop":"taille"},"lineHeight":)" +
        Decimal(rng) +
        R"(},"animate":{"opacity":{"interpolate":{"frames":[0,6],)"
        R"("values":[0,1]}}}}]}]}})");
    return {true, op};
}

const DocumentComposition* PickComposition(const Document& doc, Lcg& rng) {
    if (doc.compositions.empty()) {
        return nullptr;
    }
    return &doc.compositions[rng.NextIndex(doc.compositions.size())];
}

Ulid PickNode(const DocumentComposition& composition, Lcg& rng,
              bool includeRoot) {
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(composition.definition, nodes, keyframes);
    if (!includeRoot && !nodes.empty()) {
        nodes.erase(nodes.begin());
    }
    return nodes.empty() ? Ulid() : nodes[rng.NextIndex(nodes.size())];
}

GeneratedOp GenRemoveComposition(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    return {true, RemoveMotionCompositionOperation{composition->id}};
}

GeneratedOp GenUpdateComposition(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    DocumentComposition renamed = *composition;
    renamed.name = "Version " + std::to_string(rng.Next() % 1000);
    return {true, UpdateMotionCompositionOperation{renamed}};
}

GeneratedOp GenSetMotionNode(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    SetMotionNodeOperation op;
    op.composition_id = composition->id;
    op.parent_uid = PickNode(*composition, rng, true);
    // Seule la racine (fill) et le bloc (box) ont des enfants : un parent
    // qui n'en a pas rend une opération refusée, que I1 vérifie aussi.
    op.node = ParseJson(R"({"type":"text","content":"Ajout )" +
                        std::to_string(rng.Next() % 100) + R"("})");
    return {true, op};
}

GeneratedOp GenRemoveMotionNode(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    const Ulid node = PickNode(*composition, rng, false);
    if (node.empty()) {
        return {false, {}};
    }
    return {true, RemoveMotionNodeOperation{composition->id, node}};
}

GeneratedOp GenSetMotionStyle(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    SetMotionStyleOperation op;
    op.composition_id = composition->id;
    op.node_uid = PickNode(*composition, rng, true);
    op.property = rng.NextBool() ? "opacity" : "backgroundColor";
    if (rng.NextInt64(0, 4) != 0) {
        op.value = op.property == "opacity"
                       ? ParseJson("0." + std::to_string(rng.NextInt64(1, 9)))
                       : ParseJson(R"("#1)" +
                                   std::to_string(rng.NextInt64(10000, 99999)) +
                                   R"(")");
    }
    return {true, op};
}

GeneratedOp GenSetMotionAnimation(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    SetMotionAnimationOperation op;
    op.composition_id = composition->id;
    op.node_uid = PickNode(*composition, rng, true);
    op.property = rng.NextBool() ? "scale" : "translateY";
    if (rng.NextInt64(0, 4) != 0) {
        const int64_t end = rng.NextInt64(2, 40);
        op.animation = ParseJson(
            rng.NextBool() ? R"({"interpolate":{"frames":[0,)" +
                                 std::to_string(end) + R"(],"values":[)" +
                                 Decimal(rng) + "," + Decimal(rng) + "]}}"
                           : R"({"spring":{"from":)" + Decimal(rng) +
                                 R"(,"to":0,"damping":12.5}})");
    }
    return {true, op};
}

GeneratedOp GenSetMotionKeyframe(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(composition->definition, nodes, keyframes);
    if (keyframes.empty()) {
        return {false, {}};
    }
    SetMotionKeyframeOperation op;
    op.composition_id = composition->id;
    op.keyframe_uid = keyframes[rng.NextIndex(keyframes.size())];
    // Entre ses voisines, pour que les images restent croissantes.
    MotionKeyframeLocation location;
    if (!FindMotionKeyframe(composition->definition, op.keyframe_uid,
                            location)) {
        return {false, {}};
    }
    MotionTreePath framesPath = location.interpolate;
    framesPath.push_back({"frames", 0, false});
    const auto& frames =
        MotionTreeAt(composition->definition, framesPath)->AsArray();
    int64_t low = -1;
    int64_t high = 1000;
    if (location.index > 0) {
        frames[location.index - 1].AsInt64(low);
    }
    if (location.index + 1 < frames.size()) {
        frames[location.index + 1].AsInt64(high);
    }
    if (high - low < 2) {
        return {false, {}};
    }
    op.frame = rng.NextInt64(low + 1, high);
    op.value = ParseJson(Decimal(rng));
    return {true, op};
}

GeneratedOp GenSetMotionProps(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr) {
        return {false, {}};
    }
    SetMotionPropsOperation op;
    op.composition_id = composition->id;
    // Parfois hors bornes : un refus doit laisser le document intact (I1).
    op.props = ParseJson(
        R"({"taille":)" + std::to_string(rng.NextInt64(0, 220)) +
        R"(,"position":{"x":)" + Decimal(rng) + R"(,"y":0},"titre":"T)" +
        std::to_string(rng.Next() % 100) + R"("})");
    return {true, op};
}

// MOTION-2026-09 M4 -- un clip de motion posé après le dernier clip d'une
// piste, parfois sur une piste audio ou hors de la composition, avec des
// props d'instance parfois hors bornes : les refus doivent laisser le
// document intact (I1), les applications s'annuler octet pour octet.
GeneratedOp GenInsertMotionClip(const Document& doc, Lcg& rng) {
    const DocumentComposition* composition = PickComposition(doc, rng);
    if (composition == nullptr || doc.sequence.tracks.empty()) {
        return {false, {}};
    }
    const DocumentTrack& track =
        doc.sequence.tracks[rng.NextIndex(doc.sequence.tracks.size())];
    int64_t latestEnd = 0;
    for (const auto& clip : track.clips) {
        latestEnd =
            std::max(latestEnd, clip.timeline_in.add(clip.duration).value);
    }
    InsertMotionClipOperation op;
    op.track_id = track.id;
    op.composition_id = composition->id;
    // La carte dure 60 images : au-delà, SourceOutOfBounds.
    op.source_in = {rng.NextInt64(0, 20), kRate};
    op.duration = {rng.NextInt64(1, 50), kRate};
    op.timeline_in = {latestEnd + rng.NextInt64(0, 10), kRate};
    if (rng.NextBool()) {
        op.motion_props = ParseJson(
            R"({"taille":)" + std::to_string(rng.NextInt64(0, 220)) +
            R"(,"titre":"I)" + std::to_string(rng.Next() % 100) + R"("})");
    }
    return {true, op};
}

GeneratedOp GenSetClipMotionProps(const Document& doc, Lcg& rng) {
    std::vector<const DocumentClip*> motionClips;
    for (const DocumentClip* clip : ClipsOnTracks(doc, "video")) {
        if (IsMotionClip(*clip)) {
            motionClips.push_back(clip);
        }
    }
    if (motionClips.empty()) {
        return {false, {}};
    }
    SetClipMotionPropsOperation op;
    op.clip_id = motionClips[rng.NextIndex(motionClips.size())]->id;
    if (rng.NextInt64(0, 4) != 0) {
        op.props = ParseJson(R"({"position":{"x":)" + Decimal(rng) +
                             R"(,"y":-1},"taille":)" +
                             std::to_string(rng.NextInt64(0, 220)) + "}");
    }
    return {true, op};
}

// Rogner la tête d'un clip vidéo, média ou motion : le temps source avance.
GeneratedOp GenTrimHead(const Document& doc, Lcg& rng) {
    const auto clips = ClipsOnTracks(doc, "video");
    if (clips.empty()) return {false, {}};
    const DocumentClip& clip = *clips[rng.NextIndex(clips.size())];
    return {true,
            TrimClipOperation{
                clip.id, TrimEdge::Head, {rng.NextInt64(-5, 10), kRate}, {}}};
}

// Choisir une opération aléatoire parmi les applicables
GeneratedOp PickOperation(const Document& doc, Lcg& rng) {
    const int choice = static_cast<int>(rng.Next() % 21);
    switch (choice) {
        case 0:
            return GenAddTrack(doc, rng);
        case 1:
            return GenAddClip(doc, rng);
        case 2:
            return GenAddMarker(doc, rng);
        case 3:
            return GenRemoveClip(doc, rng);
        case 4:
            return GenRemoveMarker(doc, rng);
        case 5:
            return GenSetClipAudio(doc, rng);
        case 6:
            return GenSetClipOpacity(doc, rng);
        case 7:
            return GenMoveClip(doc, rng);
        case 8:
            return GenClearClip(doc, rng);
        case 9:
            return GenAddComposition(doc, rng);
        case 10:
            return GenRemoveComposition(doc, rng);
        case 11:
            return GenUpdateComposition(doc, rng);
        case 12:
            return GenSetMotionNode(doc, rng);
        case 13:
            return GenRemoveMotionNode(doc, rng);
        case 14:
            return GenSetMotionStyle(doc, rng);
        case 15:
            return GenSetMotionAnimation(doc, rng);
        case 16:
            return GenSetMotionKeyframe(doc, rng);
        case 17:
            return GenSetMotionProps(doc, rng);
        case 18:
            return GenInsertMotionClip(doc, rng);
        case 19:
            return GenSetClipMotionProps(doc, rng);
        case 20:
            return GenTrimHead(doc, rng);
        default:
            return {false, {}};
    }
}

// -----------------------------------------------------------------------
// Tests principaux
// -----------------------------------------------------------------------

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // T1 : invariants après chaque Apply
    // ------------------------------------------------------------------
    Test("I1-I3 : invariants préservés après N Apply valides", [] {
        Lcg rng(0xDEADBEEF42ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // D'abord on force quelques pistes pour avoir quelque chose à éditer
        for (int i = 0; i < 3; ++i) {
            GeneratedOp g = GenAddTrack(doc, rng);
            EditError err = EditError::None;
            std::string msg;
            log.Apply(doc, g.op, err, msg);
        }

        int applied = 0;
        int appliedMotionClips = 0;
        for (int step = 0; step < 200; ++step) {
            GeneratedOp g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            const std::string docBefore = doc.SaveToString();
            EditError err = EditError::None;
            std::string msg;
            const bool ok = log.Apply(doc, g.op, err, msg);

            if (ok) {
                ++applied;
                if (std::holds_alternative<InsertMotionClipOperation>(g.op) ||
                    std::holds_alternative<SetClipMotionPropsOperation>(g.op))
                    ++appliedMotionClips;
                CheckInvariants(doc, "step " + std::to_string(step));
            } else {
                // Opération rejetée : le document doit être inchangé
                Check(doc.SaveToString() == docBefore,
                      "Apply raté a muté le document (step " +
                          std::to_string(step) + ") : " + msg);
            }
        }
        // On doit avoir au moins appliqué quelques opérations
        Check(applied >= 5,
              "trop peu d'opérations applicables : " + std::to_string(applied));
        // MOTION-2026-09 M4 -- les clips de motion sont réellement exercés.
        Check(appliedMotionClips >= 2,
              "trop peu de clips de motion appliqués : " +
                  std::to_string(appliedMotionClips));
    });

    // ------------------------------------------------------------------
    // T2 : Undo restaure exactement l'état précédent (byte-identical)
    // ------------------------------------------------------------------
    Test("I4 : Undo restaure l'état précédent byte-identical", [] {
        Lcg rng(0xCAFEBABE99ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // Construire un document de base
        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int round = 0; round < 50; ++round) {
            const std::string stateBefore = doc.SaveToString();
            const size_t countBefore = log.AppliedCount();

            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            Check(log.AppliedCount() == countBefore + 1,
                  "AppliedCount n'a pas augmenté après Apply");

            // Undo
            EditError undoErr = EditError::None;
            std::string undoMsg;
            const bool undoOk = log.Undo(doc, undoErr, undoMsg);
            Check(undoOk, "Undo a échoué : " + undoMsg);
            if (undoOk) {
                Check(doc.SaveToString() == stateBefore,
                      "Undo round " + std::to_string(round) +
                          " n'a pas restauré l'état byte-identical");
                Check(log.AppliedCount() == countBefore,
                      "AppliedCount incorrect après Undo");
            }
        }
    });

    // ------------------------------------------------------------------
    // T3 : Redo restaure l'état post-Apply (byte-identical)
    // ------------------------------------------------------------------
    Test("I5 : Redo restaure l'état post-Apply byte-identical", [] {
        Lcg rng(0x1234567890ABULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int round = 0; round < 50; ++round) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;

            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            const std::string stateAfterApply = doc.SaveToString();

            // Undo
            EditError undoErr = EditError::None;
            std::string undoMsg;
            if (!log.Undo(doc, undoErr, undoMsg)) continue;

            // Redo
            EditError redoErr = EditError::None;
            std::string redoMsg;
            const bool redoOk = log.Redo(doc, redoErr, redoMsg);
            Check(redoOk, "Redo a échoué : " + redoMsg);
            if (redoOk) {
                Check(doc.SaveToString() == stateAfterApply,
                      "Redo round " + std::to_string(round) +
                          " n'a pas restauré l'état post-Apply");
            }
        }
    });

    // ------------------------------------------------------------------
    // T4 : sérialisation idempotente après N opérations
    // ------------------------------------------------------------------
    Test("I6 : Serialize(Deserialize(s)) == s après N opérations", [] {
        Lcg rng(0xFEEDFACE11ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int step = 0; step < 80; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;

            // Sérialisation du log
            const std::string s1 = log.Serialize();
            EditLog reloaded;
            EditError loadErr = EditError::None;
            std::string loadMsg;
            const bool loaded =
                EditLog::Deserialize(s1, reloaded, loadErr, loadMsg);
            Check(loaded, "Deserialize a échoué step " + std::to_string(step) +
                              " : " + loadMsg);
            if (loaded) {
                const std::string s2 = reloaded.Serialize();
                Check(s1 == s2,
                      "Serialize non idempotent step " + std::to_string(step));
            }
        }
    });

    // ------------------------------------------------------------------
    // T5 : N Apply puis N Undo => état initial (byte-identical)
    // ------------------------------------------------------------------
    Test("I7 : N Apply + N Undo => retour à l'état initial", [] {
        Lcg rng(0xABCDEF0123ULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        // Construire un document non trivial
        for (int i = 0; i < 3; ++i) {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }
        for (int i = 0; i < 5; ++i) {
            auto g = GenAddClip(doc, rng);
            if (!g.applicable) continue;
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        const std::string initialState = doc.SaveToString();
        const size_t initialApplied = log.AppliedCount();

        // Appliquer jusqu'à 30 opérations supplémentaires
        std::vector<std::string> states;
        states.push_back(initialState);
        int extraApplied = 0;

        for (int step = 0; step < 60 && extraApplied < 30; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None;
            std::string msg;
            if (!log.Apply(doc, g.op, err, msg)) continue;
            ++extraApplied;
            states.push_back(doc.SaveToString());
        }

        // Undo de tout ce qui a été appliqué en plus
        for (int i = extraApplied - 1; i >= 0; --i) {
            EditError err = EditError::None;
            std::string msg;
            const bool ok = log.Undo(doc, err, msg);
            Check(ok, "Undo #" + std::to_string(i) + " a échoué : " + msg);
            if (ok) {
                Check(doc.SaveToString() == states[static_cast<size_t>(i)],
                      "Undo #" + std::to_string(i) +
                          " : état différent de l'état attendu");
            }
        }

        Check(log.AppliedCount() == initialApplied,
              "AppliedCount incorrect après N Undo");
        Check(doc.SaveToString() == initialState,
              "N Apply + N Undo ne restaure pas l'état initial");
    });

    // ------------------------------------------------------------------
    // T6 : AppliedCount et UndoneCount sont cohérents
    // ------------------------------------------------------------------
    Test("I8 : compteurs cohérents avec Apply/Undo/Redo", [] {
        Lcg rng(0x9999DEADULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        size_t applied = log.AppliedCount();
        size_t undone = log.UndoneCount();

        Check(applied + undone == log.AppliedCount() + log.UndoneCount(),
              "invariant compteurs initial");

        for (int step = 0; step < 100; ++step) {
            const bool doUndo = rng.NextBool() && applied > 0;
            const bool doRedo = !doUndo && rng.NextBool() && undone > 0;

            if (doUndo) {
                EditError e = EditError::None;
                std::string m;
                if (log.Undo(doc, e, m)) {
                    Check(log.AppliedCount() == applied - 1,
                          "AppliedCount incorrect après Undo");
                    Check(log.UndoneCount() == undone + 1,
                          "UndoneCount incorrect après Undo");
                    applied = log.AppliedCount();
                    undone = log.UndoneCount();
                }
            } else if (doRedo) {
                EditError e = EditError::None;
                std::string m;
                if (log.Redo(doc, e, m)) {
                    Check(log.AppliedCount() == applied + 1,
                          "AppliedCount incorrect après Redo");
                    Check(log.UndoneCount() == undone - 1,
                          "UndoneCount incorrect après Redo");
                    applied = log.AppliedCount();
                    undone = log.UndoneCount();
                }
            } else {
                auto g = PickOperation(doc, rng);
                if (!g.applicable) continue;
                EditError e = EditError::None;
                std::string m;
                if (log.Apply(doc, g.op, e, m)) {
                    Check(log.AppliedCount() == applied + 1,
                          "AppliedCount incorrect après Apply");
                    // Apply vide la pile Undo
                    Check(log.UndoneCount() == 0,
                          "UndoneCount doit être 0 après Apply");
                    applied = log.AppliedCount();
                    undone = log.UndoneCount();
                }
            }
        }
    });

    // ------------------------------------------------------------------
    // T7 : Save/Load round-trip après une séquence d'opérations
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // T9 : un journal relu annule et refait comme le journal en mémoire
    // ------------------------------------------------------------------
    Test("I9 : le journal relu annule et refait byte-identical", [] {
        // QC-2026-09 A7 -- l'application relit le journal à l'ouverture d'un
        // projet et la CLI à chaque commande. Un champ que le format de
        // l'état exact oublie ne se voit ni en mémoire (I4, I5) ni dans
        // l'idempotence de la sérialisation (I6) : seulement quand on annule
        // depuis le journal relu.
        Lcg rng(0x5EED0A7A7ULL);
        Document doc = MakeBaseDocument();
        EditLog log;
        EditError err = EditError::None;
        std::string msg;
        for (const char* kind : {"video", "audio", "audio"}) {
            auto g = GenAddTrack(doc, rng);
            std::get<AddTrackOperation>(g.op).kind = kind;
            log.Apply(doc, g.op, err, msg);
        }

        int replayed = 0;
        std::set<size_t> motionKinds;
        for (int step = 0; step < 250; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            const std::string before = doc.SaveToString();
            if (!log.Apply(doc, g.op, err, msg)) continue;
            const std::string after = doc.SaveToString();
            if (IsMotionOperation(log.AppliedEntries().back().op)) {
                motionKinds.insert(log.AppliedEntries().back().op.index());
            }
            const std::string label =
                "step " + std::to_string(step) + " (" +
                SerializeOperation(log.AppliedEntries().back().op)
                    .substr(0, 32) +
                ")";

            EditLog reloaded;
            if (!EditLog::Deserialize(log.Serialize(), reloaded, err, msg)) {
                Check(false, label + " : journal illisible : " + msg);
                continue;
            }
            Document replay = doc;
            Check(reloaded.Undo(replay, err, msg) &&
                      replay.SaveToString() == before,
                  label + " : Undo du journal relu diffère : " + msg);
            Check(reloaded.Redo(replay, err, msg) &&
                      replay.SaveToString() == after,
                  label + " : Redo du journal relu diffère : " + msg);
            ++replayed;
        }
        Check(replayed >= 100,
              "trop peu d'opérations rejouées : " + std::to_string(replayed));
        // MOTION-2026-09 M3 -- chacune des neuf opérations motion.
        std::string kinds;
        for (size_t kind : motionKinds) {
            kinds += " ";
            kinds += std::to_string(kind);
        }
        Check(motionKinds.size() == 9, "opérations motion rejouées : " +
                                           std::to_string(motionKinds.size()) +
                                           " sortes sur 9 :" + kinds);
    });

    // ------------------------------------------------------------------
    // PL1 : une intervention, lot ou suite regroupée, s'annule d'un seul
    // pas, y compris depuis le journal relu
    // ------------------------------------------------------------------
    Test(
        "I9 (PL1) : le journal relu annule et refait un lot byte-identical",
        [] {
            Lcg rng(0x9A7C4B1ULL);
            Document doc = MakeBaseDocument();
            EditLog log;
            EditError err = EditError::None;
            std::string msg;
            for (const char* kind : {"video", "audio", "audio"}) {
                auto g = GenAddTrack(doc, rng);
                std::get<AddTrackOperation>(g.op).kind = kind;
                log.Apply(doc, g.op, err, msg);
            }

            int replayed = 0;
            int folded = 0;
            for (int step = 0; step < 120; ++step) {
                // Up to six operations, each generated against the state
                // the previous ones leave, applied to a scratch copy.
                Document scratch = doc;
                EditLog scratchLog;
                const int64_t count = rng.NextInt64(2, 7);
                for (int64_t index = 0; index < count; ++index) {
                    auto g = PickOperation(scratch, rng);
                    if (g.applicable) scratchLog.Apply(scratch, g.op, err, msg);
                }
                if (scratchLog.AppliedCount() < 2) continue;

                const std::string before = doc.SaveToString();
                const std::string label =
                    "step " + std::to_string(step) + " (" +
                    std::to_string(scratchLog.AppliedCount()) + " opérations)";
                if (step % 2 == 0) {
                    // Direct batch, as --apply-op receives one.
                    BatchOperation batch{label, {}};
                    for (const Entry& entry : scratchLog.AppliedEntries())
                        batch.operations.push_back(entry.op);
                    if (!log.Apply(doc, batch, err, msg)) {
                        Check(false, label + " : lot refusé : " + msg);
                        continue;
                    }
                } else {
                    // The same edits one by one, then folded.
                    const InterventionMark mark =
                        MarkIntervention(doc.sequence.id, log);
                    for (const Entry& entry : scratchLog.AppliedEntries())
                        log.Apply(doc, entry.op, err, msg);
                    size_t grouped = 0;
                    if (!log.GroupSince(doc, mark, label, grouped, err, msg)) {
                        Check(false, label + " : regroupement refusé : " + msg);
                        continue;
                    }
                    ++folded;
                }
                const std::string after = doc.SaveToString();
                Check(after == scratch.SaveToString(),
                      label + " : le lot diffère de ses membres un à un");
                Check(std::holds_alternative<BatchOperation>(
                          log.AppliedEntries().back().op),
                      label + " : l'intervention n'est pas un seul pas");

                EditLog reloaded;
                if (!EditLog::Deserialize(log.Serialize(), reloaded, err,
                                          msg)) {
                    Check(false, label + " : journal illisible : " + msg);
                    continue;
                }
                for (EditLog* history : {&log, &reloaded}) {
                    EditLog copy = *history;
                    Document replay = doc;
                    Check(copy.Undo(replay, err, msg) &&
                              replay.SaveToString() == before,
                          label + " : un Undo ne ramène pas l'état initial : " +
                              msg);
                    Check(copy.Redo(replay, err, msg) &&
                              replay.SaveToString() == after,
                          label + " : un Redo ne rejoue pas le lot : " + msg);
                }
                ++replayed;
            }
            Check(replayed >= 60 && folded >= 30,
                  "trop peu de lots rejoués : " + std::to_string(replayed) +
                      " dont " + std::to_string(folded) + " regroupés");
        });

    Test("Document Save+Load round-trip après N opérations", [] {
        Lcg rng(0xBAD1DEAULL);
        Document doc = MakeBaseDocument();
        EditLog log;

        for (int i = 0; i < 2; ++i) {
            auto g = GenAddTrack(doc, rng);
            EditError e = EditError::None;
            std::string m;
            log.Apply(doc, g.op, e, m);
        }

        for (int step = 0; step < 40; ++step) {
            auto g = PickOperation(doc, rng);
            if (!g.applicable) continue;
            EditError err = EditError::None;
            std::string msg;
            log.Apply(doc, g.op, err, msg);
        }

        // Sauvegarder via SaveToString
        const std::string saved = doc.SaveToString();

        // Recharger
        Document loaded;
        std::string loadError;
        const bool ok = Document::LoadFromString(saved, loaded, loadError);
        Check(ok, "LoadFromString a échoué : " + loadError);
        if (ok) {
            const std::string resaved = loaded.SaveToString();
            Check(resaved == saved,
                  "Document Save/Load non idempotent : "
                  "octets différents après rechargement");
        }
    });

    if (failures != 0) {
        std::cerr << failures
                  << " test(s) de séquences d'opérations échoué(s)\n";
        return 1;
    }
    std::cout << "Tous les tests de séquences d'opérations passent\n";
    return 0;
}
