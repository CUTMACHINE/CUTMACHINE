#!/usr/bin/env python3
"""Relevé d'un montage court : ce que `timeline_stats` ne compte pas.

    build/cutmachine --describe <projet> --timeline <id> | python3 releve.py
    python3 releve.py describe.json [--court 12]

Lit un instantané `describe` (la sortie de `--describe`, ou la réponse de
l'outil `describe` avec sa section `timeline`) et rend :

- les plans visibles et la distribution de leurs durées ;
- les segments visibles plus courts que `--court` images (12 par défaut) ;
- les rushes repris à distance dans le film (candidats au doublon) ;
- les traitements posés plan par plan (clés, vitesse, cadrage, opacité,
  étalonnage) et leur part ;
- les clips de motion, les transitions, les pistes son.

Lecture seule : il ne touche ni au projet ni à ses caches. `--describe`
se lit pendant qu'un serveur MCP tient le projet ; `--call-tool`, non.
Seule dépendance : Python 3, bibliothèque standard.
"""
import argparse
import json
import statistics
import sys
from collections import Counter, defaultdict

DECORATIONS = ("animation", "speed", "framing", "opacity", "effects")


def charge(chemin):
    brut = sys.stdin.read() if chemin in (None, "-") else open(chemin, encoding="utf-8").read()
    doc = json.loads(brut)
    # The `describe` tool wraps the snapshot in {"ok":..., "result": {...}};
    # the `--describe` command prints the snapshot itself, "ok":true in front
    # (PRD39).
    if "result" in doc and "timeline" not in doc:
        doc = doc["result"]
    if "timeline" not in doc:
        sys.exit("pas un instantané describe : section `timeline` absente")
    return doc


def images(valeur):
    return valeur["frames"]


def est_motion(clip):
    return bool(clip.get("composition_id"))


def vitesse_normale(clip):
    v = clip.get("speed")
    return v is None or v.get("num") == v.get("den")


def segments_visibles(pistes_video, total):
    """Top visible media clip on each frame, as (clip or None, start, length).

    Motion clips are overlays, not shots: `timeline_stats` counts them apart,
    and so does this.
    """
    dessus = [None] * total
    for piste in sorted(pistes_video, key=lambda p: p["index"]):
        if not piste.get("visible", True):
            continue
        for clip in piste["items"]:
            if clip["type"] != "clip" or est_motion(clip):
                continue
            debut = images(clip["timeline_in"])
            for f in range(debut, min(debut + images(clip["duration"]), total)):
                dessus[f] = (piste["index"], clip)
    segments = []
    for f, haut in enumerate(dessus):
        cle = haut[1]["id"] if haut else None
        if segments and segments[-1][0] == cle:
            segments[-1][3] += 1
        else:
            segments.append([cle, haut, f, 1])
    return [(haut, debut, duree) for _, haut, debut, duree in segments]


def reprises(pistes_video):
    """Sources shown in more than one run.

    A run is a string of touching clips of one track reading one source
    forward: the fragments a tightened take leaves are one run, not a reuse.
    A gap or a motion clip ends the run, since another picture came between.
    """
    passages = defaultdict(list)
    for piste in pistes_video:
        precedent = None
        for clip in piste["items"]:
            if clip["type"] != "clip" or est_motion(clip):
                precedent = None
                continue
            suite = (precedent is not None
                     and precedent["source_id"] == clip["source_id"]
                     and images(precedent["source_in"]) < images(clip["source_in"]))
            if not suite:
                passages[clip["source_id"]].append((piste["index"], clip))
            precedent = clip
    return {source: liste for source, liste in passages.items() if len(liste) > 1}


def quartiles(valeurs):
    if len(valeurs) < 2:
        return "%d" % valeurs[0]
    q1, mediane, q3 = statistics.quantiles(valeurs, n=4)
    return "min %d, quartiles %d/%d/%d, max %d" % (
        min(valeurs), round(q1), round(mediane), round(q3), max(valeurs))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("describe", nargs="?", help="fichier JSON ; l'entrée standard sinon")
    p.add_argument("--court", type=int, default=12,
                   help="seuil, en images, sous lequel un segment visible est listé")
    a = p.parse_args()

    doc = charge(a.describe)
    chrono = doc["timeline"]
    total = images(chrono["duration"])
    fichiers = {s["id"]: s["file"] for s in chrono.get("sources", [])}
    sequence = doc.get("sequence", {})
    cadence = sequence.get("frame_rate", "?")
    try:
        num, den = (int(x) for x in str(cadence).split("/"))
        ips = num / den
    except ValueError:
        ips = None

    def secondes(n):
        return (" (%.2f s)" % (n / ips)).replace(".", ",") if ips else ""

    print("%s  %sx%s@%s  %d images%s" % (
        sequence.get("name", "(sans nom)"), sequence.get("width", "?"),
        sequence.get("height", "?"), cadence, total, secondes(total)))

    pistes = chrono["tracks"]
    video = [t for t in pistes if t["kind"] == "video"]
    for t in pistes:
        clips = [c for c in t["items"] if c["type"] == "clip"]
        motion = sum(1 for c in clips if est_motion(c))
        etat = "" if t.get("visible", True) and not t.get("muted") else " (masquée ou muette)"
        detail = ", dont %d de motion" % motion if motion else ""
        print("  piste %s %d : %d clips%s%s" % (t["kind"], t["index"], len(clips), detail, etat))

    segments = segments_visibles(video, total)
    plans = [(haut, debut, duree) for haut, debut, duree in segments if haut]
    trous = [(debut, duree) for haut, debut, duree in segments if not haut]
    print()
    if plans:
        durees = [duree for _, _, duree in plans]
        print("Plans visibles : %d ; durées en images : %s" % (len(plans), quartiles(durees)))
        mode, _ = Counter(durees).most_common(1)[0]
        voisins = sum(1 for d in durees if abs(d - mode) <= 1)
        if voisins > 1:
            print("  %d plans sur %d durent %d images, à une image près"
                  % (voisins, len(durees), mode))
    else:
        print("Plans visibles : aucun (film tout en motion, ou timeline vide)")
    if trous:
        print("  trous sans image de rush : %s" % ", ".join(
            "%d+%d" % (debut, duree) for debut, duree in trous))

    courts = [(haut, debut, duree) for haut, debut, duree in plans if duree < a.court]
    print("Segments visibles de moins de %d images : %s" % (a.court, len(courts) or "aucun"))
    for (index, clip), debut, duree in courts:
        print("  image %d, %d images : %s (piste %d, clip %s)" % (
            debut, duree, fichiers.get(clip["source_id"], "?"), index, clip["id"]))

    repris = reprises(video)
    print()
    print("Rushes repris à distance : %s" % (len(repris) or "aucun"))
    for source, liste in sorted(repris.items(), key=lambda kv: -len(kv[1])):
        print("  %s, %d fois :" % (fichiers.get(source, source), len(liste)))
        for index, clip in liste:
            print("    piste %d, image %d, entrée source %d, clip %s" % (
                index, images(clip["timeline_in"]), images(clip["source_in"]), clip["id"]))

    medias = [c for t in video for c in t["items"] if c["type"] == "clip" and not est_motion(c)]
    print()
    print("Traitements, sur %d plans de rush :" % len(medias))
    parametres = Counter(piste["parameter"] for c in medias for piste in c.get("animation", []))
    comptes = {
        "animés (clés)": sum(1 for c in medias if c.get("animation")),
        "à une vitesse autre que 1": sum(1 for c in medias if not vitesse_normale(c)),
        "cadrés": sum(1 for c in medias if c.get("framing")),
        "à opacité réglée": sum(1 for c in medias if "opacity" in c),
        "étalonnés": sum(1 for c in medias if c.get("effects")),
    }
    for nom, n in comptes.items():
        part = " (%d %%)" % round(100 * n / len(medias)) if medias and n else ""
        print("  %s : %d%s" % (nom, n, part))
    if parametres:
        print("  paramètres animés : %s" % ", ".join(
            "%s sur %d plans" % (k, v) for k, v in parametres.most_common()))

    motion = [c for t in video for c in t["items"] if c["type"] == "clip" and est_motion(c)]
    if motion:
        couvert = [False] * total
        for c in motion:
            debut = images(c["timeline_in"])
            for f in range(debut, min(debut + images(c["duration"]), total)):
                couvert[f] = True
        print("Clips de motion : %d, sur %d %% des images du film"
              % (len(motion), round(100 * sum(couvert) / total) if total else 0))
    else:
        print("Clips de motion : aucun")

    transitions = doc.get("transitions")
    if transitions is None:
        print("Transitions : section absente de cet instantané")
    else:
        types = Counter(t.get("type", "?") for t in transitions)
        print("Transitions : %s" % (", ".join("%d %s" % (n, k) for k, n in types.items()) or "aucune"))

    for t in pistes:
        if t["kind"] != "audio":
            continue
        clips = [c for c in t["items"] if c["type"] == "clip"]
        if clips:
            noms = {fichiers.get(c["source_id"], "?") for c in clips}
            print("Son, piste %d : %d clip(s), %d fichier(s)%s" % (
                t["index"], len(clips), len(noms), " (muette)" if t.get("muted") else ""))


if __name__ == "__main__":
    main()
