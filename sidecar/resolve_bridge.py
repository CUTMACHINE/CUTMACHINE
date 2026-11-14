"""DaVinci Resolve Media Pool -> CUTMACHINE import manifest.

External scripting (a process driving a running Resolve from outside) is a
Studio-only capability, so this module is the one part of the pipeline that
cannot run in CI: it is split into a pure `collect()` over duck-typed folder
objects, unit-tested with fakes, and a thin `connect()` that loads Blackmagic's
module. Nothing here decides anything about media: the manifest carries
identity and organisation only -- path, name, bin -- because FFmpeg remains the
single source of truth for rate and duration on the CUTMACHINE side. Resolve
reports FPS as a float, and a float must never reach a RationalTime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import os
import sys
import unicodedata
import xml.etree.ElementTree as ElementTree
from fractions import Fraction
from typing import Any
from urllib.parse import unquote


SCHEMA = "cutmachine.resolve-manifest.v1"

MACOS_SCRIPT_API = (
    "/Library/Application Support/Blackmagic Design/DaVinci Resolve"
    "/Developer/Scripting"
)
MACOS_SCRIPT_LIB = (
    "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries"
    "/Fusion/fusionscript.so"
)


class ResolveBridgeError(RuntimeError):
    """Raised with a message meant to be shown to the user, in French."""


def connect(api_dir: str | None = None, lib_path: str | None = None) -> Any:
    """Returns the Resolve application object, or raises ResolveBridgeError."""
    api = api_dir or os.environ.get("RESOLVE_SCRIPT_API", MACOS_SCRIPT_API)
    lib = lib_path or os.environ.get("RESOLVE_SCRIPT_LIB", MACOS_SCRIPT_LIB)
    if not os.path.isdir(api):
        raise ResolveBridgeError(
            f"API de scripting Resolve introuvable : {api}. "
            "DaVinci Resolve Studio doit être installé.")
    os.environ["RESOLVE_SCRIPT_API"] = api
    os.environ["RESOLVE_SCRIPT_LIB"] = lib
    modules = os.path.join(api, "Modules")
    if modules not in sys.path:
        sys.path.append(modules)
    try:
        import DaVinciResolveScript as bmd  # type: ignore
    except ImportError as error:  # pragma: no cover - depends on the install
        raise ResolveBridgeError(
            f"Module DaVinciResolveScript illisible : {error}") from error
    resolve = bmd.scriptapp("Resolve")
    if resolve is None:
        raise ResolveBridgeError(
            "Aucune instance de Resolve ne répond. Lance DaVinci Resolve "
            "Studio, ouvre le projet, puis relance la commande. Le scripting "
            "externe est réservé à la version Studio.")
    return resolve


def collect(root: Any) -> dict[str, list[dict[str, Any]]]:
    """Walks a Media Pool folder tree into flat bin and clip lists.

    Bin keys are opaque intra-manifest references ("b0", "b1", ...) rather than
    name paths: Resolve allows '/' inside a bin name and allows two siblings to
    share a name, so a path key would need escaping and would still collide.
    The importer matches on (name, parent) anyway to stay idempotent.

    The root folder maps to the project root, which is the empty parent key --
    CUTMACHINE has no bin object for it. Media Pool entries with no file path
    (timelines, compound clips, generators) are reported as skipped rather than
    silently dropped.
    """
    bins: list[dict[str, Any]] = []
    clips: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    counter = 0

    def walk(folder: Any, key: str) -> None:
        nonlocal counter
        for clip in folder.GetClipList() or []:
            properties = clip.GetClipProperty() or {}
            path = (properties.get("File Path") or "").strip()
            name = (properties.get("Clip Name")
                    or properties.get("File Name") or "").strip()
            if not path:
                skipped.append({
                    "name": name,
                    "bin_key": key,
                    "reason": properties.get("Type") or "sans fichier",
                })
                continue
            entry = {"path": path, "name": name, "bin_key": key}
            unique_id = getattr(clip, "GetUniqueId", None)
            if callable(unique_id):
                identifier = unique_id()
                if identifier:
                    entry["resolve_uid"] = identifier
            clips.append(entry)
        for child in folder.GetSubFolderList() or []:
            counter += 1
            child_key = f"b{counter}"
            bins.append({
                "key": child_key,
                "name": child.GetName(),
                "parent_key": key,
            })
            walk(child, child_key)

    walk(root, "")
    return {"bins": bins, "clips": clips, "skipped": skipped}


def build_manifest(resolve: Any) -> dict[str, Any]:
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    media_pool = project.GetMediaPool()
    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "project": project.GetName(),
        "resolve_version": resolve.GetVersionString(),
    }
    manifest.update(collect(media_pool.GetRootFolder()))
    return manifest


TIMELINE_SCHEMA = "cutmachine.resolve-timeline.v2"


def normalized(path: str) -> str:
    """macOS hands the same path back in either Unicode normalization.

    CUTMACHINE resolves media paths through the filesystem, which yields NFD
    ("Cre" + combining acute + "puscule"); Resolve reports what it stored,
    usually NFC. Comparing the raw strings silently fails on every accented
    folder name, so both sides are folded before matching.
    """
    return unicodedata.normalize("NFC", path)


def index_media_pool(root: Any) -> dict[str, Any]:
    """Maps every clip in the Media Pool by absolute path, then by filename."""
    by_path: dict[str, Any] = {}
    by_name: dict[str, Any] = {}

    def walk(folder: Any) -> None:
        for clip in folder.GetClipList() or []:
            path = (clip.GetClipProperty("File Path") or "").strip()
            if path:
                by_path.setdefault(normalized(path), clip)
                name = (clip.GetClipProperty("File Name") or "").strip()
                if name:
                    by_name.setdefault(normalized(name), clip)
        for child in folder.GetSubFolderList() or []:
            walk(child)

    walk(root)
    # The filename fallback only helps when it is unambiguous, which the
    # setdefault above does not guarantee; callers treat it as a hint.
    return {"by_path": by_path, "by_name": by_name}


def count_srt_cues(srt_path: str) -> int:
    """Cues in an SRT, counted the way Resolve counts subtitle items."""
    with open(srt_path, encoding="utf-8-sig") as handle:
        blocks = [b for b in re.split(r"\r?\n\r?\n", handle.read().strip())
                  if b.strip()]
    return len(blocks)


# ROADMAP.md "Restent réellement ouverts" -- where the content-named copies
# of a subtitle file live. Resolve keeps a link to the imported file, so the
# copy must outlive the call: not a temporary directory, and not next to the
# caller's SRT either, which often sits on an exFAT delivery disk where macOS
# leaves AppleDouble files behind.
SRT_IMPORT_DIRECTORY = os.path.join(
    os.path.expanduser("~"), "Library", "Application Support", "CUTMACHINE",
    "resolve-srt")


def content_named_srt(srt_path: str, directory: str | None = None) -> str:
    """Copy of `srt_path` whose file name carries a digest of its bytes.

    Resolve caches imported media by path: importing a rewritten .srt hands
    back the Media Pool item of the previous version, and the timeline
    receives the old cues without any error (seven of eight montages sent
    back on 16 September). A name derived from the content changes whenever
    the cues change and stays the same when they do not, so an item Resolve
    hands back from its cache can only hold these same subtitles.
    """
    with open(srt_path, "rb") as handle:
        data = handle.read()
    digest = hashlib.sha256(data).hexdigest()[:16]
    stem = os.path.splitext(os.path.basename(srt_path))[0] or "sous-titres"
    target_directory = directory or SRT_IMPORT_DIRECTORY
    target = os.path.join(target_directory, f"{stem}-{digest}.srt")
    try:
        os.makedirs(target_directory, exist_ok=True)
        existing = None
        if os.path.exists(target):
            with open(target, "rb") as handle:
                existing = handle.read()
        if existing != data:
            partial = target + ".partial"
            with open(partial, "wb") as handle:
                handle.write(data)
            os.replace(partial, target)
    except OSError as error:
        raise ResolveBridgeError(
            f"Impossible de préparer la copie du SRT à importer dans "
            f"{target_directory} : {error}") from error
    return target


def count_subtitle_items(timeline: Any) -> int:
    """Subtitle items on every subtitle track of `timeline`.

    AppendToTimeline ignores trackIndex for subtitles and stacks every cue on
    S1, whatever track was asked for: counting only the track the bridge
    means to fill could miss what actually landed, so every subtitle track
    is summed.
    """
    total = 0
    for index in range(1, int(timeline.GetTrackCount("subtitle") or 0) + 1):
        total += len(timeline.GetItemListInTrack("subtitle", index) or [])
    return total


def send_timeline(resolve: Any, timeline: dict[str, Any],
                  name: str | None = None,
                  srt_path: str | None = None,
                  srt_directory: str | None = None) -> str:
    """Rebuilds a CUTMACHINE cut as a new Resolve timeline.

    `name` overrides the montage's own name. Resolve refuses a duplicate
    timeline name, so without an override a cut can only ever be sent once:
    every later revision of the same montage would be rejected. This is where
    the operator says where it lands, not what it contains.

    `srt_path` adds a subtitle track. It is appended *first*, while the
    timeline is still empty: AppendToTimeline puts a subtitle element after
    whatever the timeline already holds, not at the start of its own track, so
    subtitling a filled timeline lands every cue past the last clip and
    doubles the timeline's length without raising. `recordFrame` does not
    work around it -- it is ignored on a subtitle element -- so the order
    below is the only way. The clipInfo carries nothing but the item and the
    track: adding startFrame, endFrame, recordFrame or mediaType crashes the
    bridge.

    The subtitles are imported from a copy named after their content (see
    content_named_srt, written under `srt_directory`), and the items placed
    on the subtitle tracks are then counted against the cues of the file: a
    difference raises, naming both counts, before any clip is added.
    """
    if timeline.get("schema") != TIMELINE_SCHEMA:
        raise ResolveBridgeError(
            f"schéma inattendu : {timeline.get('schema')} "
            f"(attendu {TIMELINE_SCHEMA})")
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    media_pool = project.GetMediaPool()
    index = index_media_pool(media_pool.GetRootFolder())

    pending = []
    missing = []
    layers = 1
    for clip in timeline.get("clips", []):
        item = index["by_path"].get(normalized(clip["path"]))
        if item is None:
            item = index["by_name"].get(normalized(clip.get("filename", "")))
        if item is None:
            missing.append(clip.get("filename") or clip["path"])
            continue
        layer = int(clip.get("video_layer", 0))
        layers = max(layers, layer + 1)
        info = {
            "mediaPoolItem": item,
            "startFrame": int(clip["start_frame"]),
            "endFrame": int(clip["end_frame"]),
            "trackIndex": layer + 1,
        }
        if not clip.get("with_audio", False):
            # mediaType 1 = image seule. Sans ça, Resolve rapporte le son du
            # plan de coupe et le pose sous l'interview qu'il recouvre.
            info["mediaType"] = 1
        pending.append((info, int(clip.get("record_frame", 0))))
    if missing:
        raise ResolveBridgeError(
            "Rushes absents du Media Pool : " + ", ".join(sorted(set(missing))))
    if not pending:
        raise ResolveBridgeError("Aucun plan à envoyer.")

    cues = 0
    import_path = None
    if srt_path:
        # Both read before the timeline exists, so an unreadable file leaves
        # nothing behind in Resolve.
        try:
            cues = count_srt_cues(srt_path)
        except OSError as error:
            raise ResolveBridgeError(
                f"SRT illisible : {srt_path} ({error})") from error
        import_path = content_named_srt(srt_path, srt_directory)

    name = name or timeline.get("name") or "CUTMACHINE"
    created = media_pool.CreateEmptyTimeline(name)
    if not created:
        raise ResolveBridgeError(
            f"Resolve a refusé de créer la timeline « {name} » "
            "(un nom identique existe peut-être déjà).")
    if import_path:
        if not created.AddTrack("subtitle"):
            raise ResolveBridgeError(
                "Resolve a refusé d'ajouter une piste de sous-titres.")
        # Resolve caches imported media by path, so a rewritten .srt would
        # come back as the previous version: the copy imported here is named
        # after its content, and the count below checks what was placed.
        before = count_subtitle_items(created)
        imported = media_pool.ImportMedia([import_path]) or []
        if not imported:
            raise ResolveBridgeError(
                f"Resolve n'a pas importé le fichier de sous-titres : "
                f"{import_path} (copie de {srt_path})")
        if not media_pool.AppendToTimeline(
                [{"mediaPoolItem": imported[0], "trackIndex": 1}]):
            raise ResolveBridgeError(
                "AppendToTimeline a refusé les sous-titres.")
        placed = count_subtitle_items(created) - before
        if placed != cues:
            raise ResolveBridgeError(
                f"Resolve a posé {placed} sous-titre(s) sur la timeline "
                f"« {name} », alors que {srt_path} compte {cues} cue(s) : "
                "ce ne sont pas les sous-titres du fichier (un ancien SRT "
                "repris du Media Pool, ou des cues fusionnées à l'import). "
                f"Aucun plan n'a été posé ; supprime la timeline « {name} » "
                "dans Resolve avant de renvoyer.")
    # Une timeline neuve n'a qu'une piste vidéo : chaque couche de recouvrement
    # a besoin de la sienne avant qu'AppendToTimeline puisse l'adresser.
    while int(created.GetTrackCount("video")) < layers:
        if not created.AddTrack("video"):
            raise ResolveBridgeError(
                "Resolve a refusé d'ajouter une piste vidéo.")
    # Resolve compte les images depuis le timecode de départ de la séquence
    # (90000 pour 01:00:00:00 à 25 i/s) ; CUTMACHINE compte depuis zéro. Sans
    # ce décalage, les plans repartiraient bout à bout au lieu de se poser où
    # le montage les place.
    start = int(created.GetStartFrame())
    infos = []
    for info, record_frame in pending:
        info["recordFrame"] = start + record_frame
        infos.append(info)
    if not media_pool.AppendToTimeline(infos):
        raise ResolveBridgeError("AppendToTimeline a échoué.")
    couches = "" if layers == 1 else f", sur {layers} pistes vidéo"
    titres = ("" if not import_path else
              f", {cues} sous-titre(s) importés depuis "
              f"{os.path.basename(import_path)}")
    return (f"{len(infos)} plan(s) → timeline « {name} » dans Resolve"
            f"{couches}{titres}")


# R9 -- docs/roadmap/REEL-2026-09.md. L'autre renvoi : une timeline importée
# depuis le xmeml v5 que `cutmachine --export-resolve-xml` écrit, parce
# qu'AppendToTimeline ne porte ni vitesse ni transition (mesuré sur 20.3, le
# 19 septembre, et confirmé le 22).
#
# Ce que le pont ajoute au fichier, et pourquoi il ne peut pas venir du
# moteur. Mesuré le 22 septembre sur 20.3.1 : avec `importSourceClips` à
# faux, Resolve apparie un <file> à un clip du Media Pool **par son timecode
# de départ, et par lui seul**. Un <file> qui porte le chemin et le nom d'un
# rush et le timecode d'un autre arrive sur l'autre, et le plan de la
# timeline prend même son nom ; un <file> sans timecode, ou à 00:00:00:00,
# tombe sur n'importe quel média non timecodé des chutiers fouillés. Le
# document ne porte pas le timecode d'un média (ce n'est pas un fait de
# montage, et R9 n'ajoute rien au format), donc le moteur écrit un marqueur
# qui n'apparie rien et c'est ici qu'on le remplace, depuis le Media Pool.
#
# Et parce qu'un appariement par timecode se trompe en silence, l'import est
# relu : chaque plan posé doit porter le média que le montage nommait, à la
# position que le montage nommait.
XML_SCHEMA_MARKER = "cutmachine.resolve-xml.v1"

# Où vivent les copies complétées. Pas un dossier temporaire : quand un
# import se passe mal, le fichier réellement donné à Resolve est ce qu'on
# relit pour comprendre.
XML_IMPORT_DIRECTORY = os.path.join(
    os.path.expanduser("~"), "Library", "Application Support", "CUTMACHINE",
    "resolve-xml")


def media_pool_folders(root: Any) -> list[Any]:
    """Tous les chutiers du Media Pool, racine comprise.

    `sourceClipsFolders` ne fouille pas ce qu'on ne lui donne pas, et une
    liste partielle ne se solde pas par un refus mais par un appariement
    silencieusement faux : mesuré le 22 septembre, un import dont la liste
    omettait le chutier des rushes a lié les six plans à deux fichiers de
    motion qui partagent le timecode 00:00:00:00.
    """
    folders = [root]
    for child in root.GetSubFolderList() or []:
        folders.extend(media_pool_folders(child))
    return folders


def xml_file_path(element: Any) -> str:
    """Le chemin d'un <file>, depuis son <pathurl>."""
    url = (element.findtext("pathurl") or "").strip()
    if not url.startswith("file://"):
        raise ResolveBridgeError(
            f"<file> sans chemin lisible dans le XML : {url!r}")
    return unquote(url[len("file://"):])


def read_xml_timeline(
        tree: Any) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Les <file> décrits et, par piste vidéo, les plans dans l'ordre.

    Pure : ne touche ni Resolve ni le disque, pour que le remplissage des
    timecodes et la relecture se testent avec un arbre construit à la main.
    """
    sequence = tree.getroot().find("sequence")
    if sequence is None:
        raise ResolveBridgeError(
            "XML sans <sequence> : ce n'est pas un xmeml.")
    video = sequence.find("media/video")
    if video is None:
        raise ResolveBridgeError("XML sans piste vidéo.")
    files: dict[str, Any] = {}
    tracks: list[dict[str, Any]] = []
    for track in video.findall("track"):
        clips: list[dict[str, Any]] = []
        transitions = 0
        for child in track:
            if child.tag == "clipitem":
                element = child.find("file")
                if element is None or not element.get("id"):
                    raise ResolveBridgeError(
                        "clipitem sans <file> : le XML ne dit pas quel rush "
                        "poser.")
                identifier = element.get("id")
                if element.find("pathurl") is not None:
                    files[identifier] = element
                clips.append({
                    "file": identifier,
                    "start": int(child.findtext("start")),
                    "end": int(child.findtext("end")),
                })
            elif child.tag == "transitionitem":
                transitions += 1
        tracks.append({"clips": clips, "transitions": transitions})
    for track in tracks:
        for clip in track["clips"]:
            if clip["file"] not in files:
                raise ResolveBridgeError(
                    f"le XML référence le média « {clip['file']} » sans "
                    "jamais le décrire.")
    return files, tracks


def stamp_xml_timecodes(files: dict[str, Any],
                        lookup: Any) -> dict[str, str]:
    """Remplace le timecode marqueur de chaque <file> par celui du média.

    `lookup(chemin)` rend `(timecode, cadence entière)` ou None. Un chemin
    absent du Media Pool est refusé nommément : l'importer quand même
    poserait un plan hors ligne, ou pire, sur un autre rush. Deux médias qui
    partagent un timecode sont refusés aussi, parce que Resolve n'a alors
    aucun moyen de les distinguer.
    """
    missing: list[str] = []
    stamped: dict[str, str] = {}
    by_timecode: dict[str, str] = {}
    collisions: list[str] = []
    for identifier in sorted(files):
        element = files[identifier]
        path = xml_file_path(element)
        found = lookup(path)
        if found is None:
            missing.append(path)
            continue
        timecode, rate = found
        if not timecode:
            missing.append(path)
            continue
        if timecode in by_timecode and by_timecode[timecode] != path:
            collisions.append(
                f"{os.path.basename(by_timecode[timecode])} et "
                f"{os.path.basename(path)} à {timecode}")
        by_timecode.setdefault(timecode, path)
        node = element.find("timecode")
        if node is None:
            node = ElementTree.SubElement(element, "timecode")
        string = node.find("string")
        if string is None:
            string = ElementTree.SubElement(node, "string")
        string.text = timecode
        timebase = node.find("rate/timebase")
        if timebase is not None:
            timebase.text = str(rate)
        stamped[identifier] = path
    if missing:
        raise ResolveBridgeError(
            "Rushes absents du Media Pool (ou sans timecode de départ) : "
            + ", ".join(sorted(set(missing)))
            + ". Importe-les d'abord ; Resolve apparie un média du XML par "
              "son timecode, donc sans eux l'import tomberait sur un autre "
              "rush sans le dire.")
    if collisions:
        raise ResolveBridgeError(
            "Deux rushes du montage partagent un timecode de départ : "
            + ", ".join(sorted(set(collisions)))
            + ". Resolve les apparie par ce timecode et ne peut pas les "
              "distinguer ; le renvoi par XML est impossible tel quel.")
    return stamped


def verify_imported_timeline(timeline: Any, tracks: list[dict[str, Any]],
                             paths: dict[str, str]) -> int:
    """Relit la timeline posée et compare chaque plan à ce que le XML disait.

    L'appariement par timecode se trompe en silence : sans cette relecture,
    un montage renvoyé sur les mauvais rushes passerait pour réussi.
    """
    origin = int(timeline.GetStartFrame())
    placed = 0
    for index, track in enumerate(tracks, start=1):
        items = timeline.GetItemListInTrack("video", index) or []
        linked = [item for item in items if item.GetMediaPoolItem()]
        expected = track["clips"]
        if len(linked) != len(expected):
            raise ResolveBridgeError(
                f"V{index} : {len(linked)} plan(s) reliés à un média pour "
                f"{len(expected)} attendu(s). Un plan est resté hors ligne, "
                "ou Resolve n'a pas trouvé son timecode.")
        for item, clip in zip(linked, expected):
            start = int(item.GetStart()) - origin
            if start != clip["start"]:
                raise ResolveBridgeError(
                    f"V{index} : un plan est posé à l'image {start} alors "
                    f"que le montage le place à {clip['start']}.")
            media = item.GetMediaPoolItem()
            got = normalized(
                (media.GetClipProperty("File Path") or "").strip())
            want = normalized(paths[clip["file"]])
            if got != want:
                raise ResolveBridgeError(
                    f"V{index}, image {start} : Resolve a posé "
                    f"« {os.path.basename(got)} » là où le montage nomme "
                    f"« {os.path.basename(want)} ». Resolve apparie un média "
                    "du XML par son timecode de départ : deux rushes doivent "
                    "partager le leur.")
            placed += 1
        if len(items) - len(linked) != track["transitions"]:
            raise ResolveBridgeError(
                f"V{index} : {len(items) - len(linked)} élément(s) sans média "
                f"pour {track['transitions']} transition(s) attendue(s).")
    return placed


def count_media_pool(folder: Any) -> int:
    total = len(folder.GetClipList() or [])
    for child in folder.GetSubFolderList() or []:
        total += count_media_pool(child)
    return total


def send_timeline_xml(resolve: Any, xml_path: str, name: str | None = None,
                      directory: str | None = None) -> str:
    """Importe dans Resolve le xmeml écrit par --export-resolve-xml.

    Le SRT n'a pas sa place ici, et c'est mesuré : le xmeml ne porte pas de
    piste de sous-titres (l'export FCP7 de Resolve lui-même n'en écrit
    aucune trace), et une timeline importée arrive pleine, si bien
    qu'AppendToTimeline décale chaque cue de la longueur du montage -- 160
    images de montage, cues attendues à 10, 50 et 100, posées à 170, 210 et
    260, `recordFrame` ignoré. Les sous-titres restent donc sur le renvoi par
    AppendToTimeline, qui exige une timeline vide.
    """
    tree = ElementTree.parse(xml_path)
    files, tracks = read_xml_timeline(tree)
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    media_pool = project.GetMediaPool()
    root = media_pool.GetRootFolder()
    index = index_media_pool(root)

    def lookup(path: str) -> tuple[str, int] | None:
        item = index["by_path"].get(normalized(path))
        if item is None:
            item = index["by_name"].get(
                normalized(os.path.basename(path)))
        if item is None:
            return None
        try:
            return ((item.GetClipProperty("Start TC") or "").strip(),
                    integral_rate(item.GetClipProperty("FPS")))
        except ResolveBridgeError:
            return None

    paths = stamp_xml_timecodes(files, lookup)
    name = name or (tree.getroot().find("sequence").findtext("name")
                    or "CUTMACHINE")
    for existing in range(1, int(project.GetTimelineCount()) + 1):
        if project.GetTimelineByIndex(existing).GetName() == name:
            raise ResolveBridgeError(
                f"Une timeline « {name} » existe déjà dans Resolve ; "
                "choisis un autre nom avec --name plutôt que d'en poser une "
                "seconde du même nom.")

    target_directory = directory or XML_IMPORT_DIRECTORY
    try:
        os.makedirs(target_directory, exist_ok=True)
        stamped_path = os.path.join(target_directory, f"{name}.xml")
        with open(stamped_path, "wb") as handle:
            handle.write(b'<?xml version="1.0" encoding="UTF-8"?>\n'
                         b"<!DOCTYPE xmeml>\n")
            tree.write(handle, encoding="utf-8", xml_declaration=False)
    except OSError as error:
        raise ResolveBridgeError(
            f"Impossible d'écrire le XML complété dans {target_directory} : "
            f"{error}") from error

    before = count_media_pool(root)
    timeline = media_pool.ImportTimelineFromFile(stamped_path, {
        "timelineName": name,
        "importSourceClips": False,
        "sourceClipsFolders": media_pool_folders(root),
    })
    if not timeline:
        raise ResolveBridgeError(
            f"Resolve a refusé d'importer {stamped_path}.")
    after = count_media_pool(root)
    placed = verify_imported_timeline(timeline, tracks, paths)
    # Le Media Pool ne doit gagner que la timeline elle-même : c'est ce qui
    # distingue cette voie du FCPXML, qui fabrique un clip composé par plan
    # ralenti. Un gain nul n'est pas une faute (Resolve peut ne pas classer
    # la timeline dans le chutier), un gain de deux en est une.
    gained = after - before
    if gained > 1:
        raise ResolveBridgeError(
            f"Le Media Pool a gagné {gained} éléments au lieu de la seule "
            f"timeline : des rushes ont été dupliqués. Supprime la timeline "
            f"« {name} » et ses doublons avant de renvoyer.")
    transitions = sum(track["transitions"] for track in tracks)
    couches = "" if len(tracks) == 1 else f", sur {len(tracks)} pistes vidéo"
    fondus = "" if not transitions else f", {transitions} transition(s)"
    return (f"{placed} plan(s) → timeline « {name} » dans Resolve"
            f"{couches}{fondus} ; Media Pool +{gained} (la timeline), "
            f"{placed} plan(s) relus et vérifiés")


FLATTEN_SCHEMA = "cutmachine.resolve-flatten.v1"


def integral_rate(text: str) -> int:
    """Resolve reports a frame rate as a float string; only integers survive.

    PHILOSOPHY.md principle 4 forbids a float from becoming a rate, so this
    never builds one: it accepts the string only when it denotes a whole
    number, and refuses everything else rather than rounding 23.976 into 24.
    The value is used to *check* agreement between the montage and the
    flattening map, never to construct a RationalTime.
    """
    try:
        value = float(text)
    except (TypeError, ValueError):
        raise ResolveBridgeError(f"cadence illisible : {text!r}")
    if value != int(value):
        raise ResolveBridgeError(
            f"cadence non entière ({text}) : le remappage vers les rushes "
            "sources n'est exact qu'à cadence entière.")
    return int(value)


def item_speed(item: Any, media: Any, timeline_rate: int,
               where: str) -> Fraction:
    """Exact playback speed of a timeline item, as a fraction. Never guesses.

    Measured on Resolve 20.3.1, 22 September 2026, on one rush of 1776 frames
    at 50 i/s in a 25 i/s timeline:

    | clip            | left | span | right | total |
    |-----------------|------|------|-------|-------|
    | 100%, untrimmed |    0 |   83 |   805 |   888 |
    | 50%,  untrimmed |    0 |   83 |  1693 |  1776 |
    | 100%, trimmed   |  200 |  199 |   489 |   888 |

    GetLeftOffset and GetRightOffset are in *timeline* frames and already
    account for the speed, so `left + span + right` is how many timeline
    frames the whole media occupies at the clip's speed. The same media at
    speed 1 occupies `frames * timeline_rate / source_rate`. The ratio of the
    two is the speed, exactly, from integers alone -- 888/888 = 1 and
    888/1776 = 1/2 above, with no rounding anywhere.

    Why not the obvious routes. GetDuration equals GetEnd-GetStart on a
    retimed clip just as on a normal one (83 and 83 above), which is what made
    the old guard below vacuous. GetSourceEndFrame does differ (165 against
    83) but is short of the true span by one or two frames depending on the
    clip, as `flatten_timeline` already records, so it identifies a retime
    without ever giving an exact ratio. And no property published by
    GetProperty carries a speed at all.

    Raises rather than returning a default: a clip whose speed cannot be
    measured is exactly the clip that must not be assumed normal.
    """
    for name in ("GetLeftOffset", "GetRightOffset"):
        if not callable(getattr(item, name, None)):
            raise ResolveBridgeError(
                f"{where} : {name} absent de ce build de Resolve ; la vitesse "
                "du plan ne peut pas être mesurée, et la supposer normale est "
                "ce qui décale les images en silence.")
    if media is None:
        raise ResolveBridgeError(
            f"{where} : pas de média derrière ce plan, donc aucune vitesse "
            "mesurable.")
    frames_text = media.GetClipProperty("Frames")
    try:
        frames = int(frames_text)
    except (TypeError, ValueError):
        raise ResolveBridgeError(
            f"{where} : nombre d'images du média illisible "
            f"({frames_text!r}).") from None
    # integral_rate, not a float: the media cadence divides a frame count
    # here, and 23.976 rounded into 24 would move the result by a frame per
    # thousand -- enough to read a normal clip as retimed.
    source_rate = integral_rate(media.GetClipProperty("FPS"))
    span = int(item.GetEnd()) - int(item.GetStart())
    total = int(item.GetLeftOffset()) + span + int(item.GetRightOffset())
    if total <= 0 or frames <= 0:
        raise ResolveBridgeError(
            f"{where} : étendue nulle (total {total}, média {frames}).")
    return Fraction(frames * timeline_rate, source_rate * total)


def flatten_timeline(timeline: Any) -> dict[str, Any]:
    """Maps a Resolve timeline to the source frame behind every output frame.

    RESOLVE-2026-09 -- a flattened render is what CUTMACHINE can hear, and the
    original rushes are what the montage must come back on. That round trip
    needs one fact per output frame: which source file it came from, and at
    which frame in it. This produces exactly that, as contiguous segments.

    Bounds come from GetStart/GetEnd/GetDuration and the in-point from
    GetSourceStartFrame. GetSourceEndFrame is deliberately unused: measured
    across the six PRJC interviews it disagrees with the item duration
    by one frame on some clips and two on others (C125: dur 216, span 215;
    C126: dur 138, span 136), while GetEnd-GetStart equals GetDuration on
    every single item. Deriving the source length from it would silently drop
    a frame or two per clip.

    Refuses rather than guesses: a second populated video track has no defined
    flattening, and a gap, a retime or an item with no file on disk each break
    the frame correspondence this exists to guarantee.
    """
    name = timeline.GetName()
    origin = int(timeline.GetStartFrame())
    rate = integral_rate(timeline.GetSetting("timelineFrameRate"))

    populated = []
    for track in range(1, int(timeline.GetTrackCount("video")) + 1):
        if timeline.GetItemListInTrack("video", track):
            populated.append(track)
    if not populated:
        raise ResolveBridgeError(f"« {name} » n'a aucun plan vidéo.")
    if len(populated) > 1:
        raise ResolveBridgeError(
            f"« {name} » a des plans sur {len(populated)} pistes vidéo. "
            "Un rendu à plat ne dit pas quelle couche est visible à chaque "
            "image : aplatis la timeline sur une seule piste, ou renonce au "
            "retour sur les rushes sources pour celle-ci.")

    segments: list[dict[str, Any]] = []
    cursor = origin
    for item in timeline.GetItemListInTrack("video", populated[0]):
        start, end = int(item.GetStart()), int(item.GetEnd())
        label = item.GetName()
        if start != cursor:
            raise ResolveBridgeError(
                f"« {name} » : trou de {start - cursor} image(s) avant "
                f"{label}. Un trou n'a aucun rush derrière lui.")
        media = item.GetMediaPoolItem()
        path = (media.GetClipProperty("File Path") or "").strip() if media \
            else ""
        if not path:
            raise ResolveBridgeError(
                f"« {name} » : le plan {label} n'a pas de fichier sur le "
                "disque (timeline imbriquée, clip composé ou générateur).")
        # Measured 22 September 2026: this used to compare `end - start`
        # against GetDuration, and Resolve reports those equal on a retimed
        # clip exactly as on a normal one (83 and 83 on a clip slowed to
        # 50%). The guard therefore never fired, and a slowed clip went
        # through this flattening with a frame correspondence that was
        # silently wrong -- the one thing this function exists to prevent.
        # The speed is now measured; see item_speed.
        speed = item_speed(item, media, rate,
                           f"« {name} » : le plan {label}")
        if speed != 1:
            raise ResolveBridgeError(
                f"« {name} » : le plan {label} est à la vitesse {speed} "
                f"({float(speed):.4g}). Le remappage vers la source suppose "
                "une vitesse normale.")
        segments.append({
            "record_start": start - origin,
            "record_end": end - origin,
            "source_start": int(item.GetSourceStartFrame()),
            "path": path,
            "filename": os.path.basename(path),
        })
        cursor = end

    return {
        "name": name,
        "frame_rate": rate,
        "duration": cursor - origin,
        "segments": segments,
    }


def read_flattening(resolve: Any, prefix: str) -> dict[str, Any]:
    """Flattens every timeline whose name starts with `prefix`."""
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    timelines = []
    for index in range(1, int(project.GetTimelineCount()) + 1):
        timeline = project.GetTimelineByIndex(index)
        if timeline.GetName().startswith(prefix):
            timelines.append(flatten_timeline(timeline))
    if not timelines:
        raise ResolveBridgeError(
            f"Aucune timeline ne commence par « {prefix} ».")
    timelines.sort(key=lambda entry: entry["name"])
    return {
        "schema": FLATTEN_SCHEMA,
        "project": project.GetName(),
        "timelines": timelines,
    }


def remap_to_sources(montage: dict[str, Any],
                     flattening: dict[str, Any]) -> dict[str, Any]:
    """Rewrites a cut made on a flattened render onto the original rushes.

    RESOLVE-2026-09 -- CUTMACHINE cannot decode BRAW, so the montage is built
    on a ProRes render of the whole interview. Its clips address that render's
    frames; Resolve must receive the rushes instead. A cut that straddles a
    join in the source timeline covers two rushes, so one montage clip can
    come back as several -- which is why this splits rather than translates.

    Both sides must agree on the cadence: the offsets carried here are frame
    counts, and a montage at another rate would move every cut.
    """
    if montage.get("schema") != TIMELINE_SCHEMA:
        raise ResolveBridgeError(
            f"schéma de montage inattendu : {montage.get('schema')}")
    rate = montage.get("frame_rate") or {}
    if int(rate.get("den", 0)) != 1 or \
            int(rate.get("num", 0)) != int(flattening["frame_rate"]):
        raise ResolveBridgeError(
            f"le montage est à {rate.get('num')}/{rate.get('den')} i/s et la "
            f"timeline source à {flattening['frame_rate']} : le remappage "
            "déplacerait les coupes.")

    segments = flattening["segments"]
    remapped: list[dict[str, Any]] = []
    for clip in montage.get("clips", []):
        start, end = int(clip["start_frame"]), int(clip["end_frame"])
        covered = 0
        for segment in segments:
            low = max(start, segment["record_start"])
            high = min(end, segment["record_end"])
            if low >= high:
                continue
            offset = low - segment["record_start"]
            source_in = segment["source_start"] + offset
            remapped.append({
                "path": segment["path"],
                "filename": segment["filename"],
                "start_frame": source_in,
                "end_frame": source_in + (high - low),
                "video_layer": int(clip.get("video_layer", 0)),
                # The montage's own position shifts by the same number of
                # frames as the piece taken from it, because both domains run
                # at the rate checked above.
                "record_frame": int(clip["record_frame"]) + (low - start),
                "with_audio": bool(clip.get("with_audio", False)),
            })
            covered += high - low
        if covered != end - start:
            raise ResolveBridgeError(
                f"un plan du montage ({start}-{end}) sort du rendu "
                f"cartographié ({covered} image(s) couverte(s) sur "
                f"{end - start}).")
    if not remapped:
        raise ResolveBridgeError("Aucun plan à remapper.")

    result = dict(montage)
    result["clips"] = remapped
    return result


# R10 -- docs/roadmap/REEL-2026-09.md. Reading a timeline back, the other
# direction of --send. Same division of labour as the Media Pool manifest
# above: this *records* what Resolve says, and refuses nothing but what it
# cannot read at all; the engine (src/ResolveImport.h) parses, plans and
# names every refusal.
#
# Nothing here carries a float. Positions are whole frames, framing values
# travel as their own text, and FFmpeg stays the single source of truth for
# cadence and duration on the CUTMACHINE side.
#
# The schema name is deliberately not "cutmachine.resolve-timeline.v1": that
# string is the retired *send* schema that TIMELINE_SCHEMA's v2 replaced, and
# a reader that accepted it would read a stale montage descriptor as a
# timeline to import.
TIMELINE_READ_SCHEMA = "cutmachine.resolve-timeline-read.v1"

# Item properties whose default proves a clip carries no framing and no
# compositing. The engine holds the same table with the default of each
# (ResolveImport.h) and refuses, by name, both a value that is not its
# default and a key it does not know -- so the two sides cannot drift apart
# in silence.
#
# Two blind spots, measured on Resolve 20.3: GetProperty returns 0 on an
# *animated* parameter, so an animated zoom arrives as ZoomX=0 and is refused
# because 0 is not the default 1, while an animated Pan or Tilt reads 0,
# which is its default, and passes unseen. And the API publishes no
# transition at all.
# Measured against Resolve 20.3.1 on 22 September 2026, by dumping
# GetProperty() on an untouched clip: these are the 26 keys it publishes, and
# the values below are what an untouched clip reports. The earlier table was
# written from guesses and carried a key Resolve does not have -- "Dynamic
# Zoom" -- which `item_properties` skipped in silence, so a clip carrying a
# dynamic zoom read as a clip carrying no framing at all. A guessed key is
# worse than a missing one here: it looks like coverage.
#
# No speed key exists among the 26. RetimeProcess is the retime *algorithm*
# (nearest, frame blend, optical flow), identical on a clip at 100% and on the
# same clip at 50%; it says nothing about the ratio. That is why the speed is
# derived from the offsets instead (see `item_speed`).
FRAMING_PROPERTIES = (
    "ZoomX", "ZoomY", "Pan", "Tilt", "RotationAngle",
    "AnchorPointX", "AnchorPointY",
    "CropLeft", "CropRight", "CropTop", "CropBottom", "CropSoftness",
    "CropRetain", "FlipX", "FlipY", "Opacity", "CompositeMode",
    "DynamicZoomEase", "Distortion", "Pitch", "Yaw", "ResizeFilter",
    "Scaling", "ZoomGang", "MotionEstimation", "RetimeProcess",
)

# What the scripting API does not publish, carried in the manifest so the
# engine's report can name it instead of letting a reader assume the timeline
# held none of it. Closing these is what the .drt route would buy.
# Corrected 22 September 2026 against a real montage. The speed came off
# this list: it is derived exactly from the offsets (see speed_facts). So did
# the transitions -- GetItemListInTrack returns them as items of their own,
# named after their kind ("Cross Dissolve"), carrying no media pool item and
# overlapping the clips they join; `read_transition` records them and the
# engine reads a cross dissolve as a DocumentTransition (R10).
UNPUBLISHED_BY_API = (
    "les images clés d'un cadrage (GetProperty rend 0 sur un paramètre animé)",
)


def property_text(value: Any) -> str:
    """A Resolve property as text: a float never enters the manifest."""
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value).strip()


def item_properties(item: Any) -> dict[str, str]:
    """The framing properties Resolve reports for one timeline item.

    GetProperty answers the whole bag with no argument and one value with a
    key; builds that only accept the second form are handled by falling back
    to one call per key, because silently returning nothing here would let a
    framed clip through as if it carried none.
    """
    getter = getattr(item, "GetProperty", None)
    if not callable(getter):
        return {}
    try:
        bag = getter()
    except TypeError:
        bag = None
    if not isinstance(bag, dict):
        bag = {}
        for key in FRAMING_PROPERTIES:
            try:
                bag[key] = getter(key)
            except (TypeError, ValueError):
                continue
    properties: dict[str, str] = {}
    for key in FRAMING_PROPERTIES:
        if key not in bag:
            continue
        value = bag[key]
        if value is None or value == "":
            continue
        properties[key] = property_text(value)
    return properties


def fusion_comp_count(item: Any) -> int:
    counter = getattr(item, "GetFusionCompCount", None)
    if not callable(counter):
        return 0
    try:
        return int(counter() or 0)
    except (TypeError, ValueError):
        return 0


def track_name(timeline: Any, kind: str, index: int) -> str:
    getter = getattr(timeline, "GetTrackName", None)
    if not callable(getter):
        return ""
    return (getter(kind, index) or "").strip()


def integral_setting(timeline: Any, key: str) -> int:
    text = (timeline.GetSetting(key) or "").strip()
    try:
        value = int(text)
    except (TypeError, ValueError):
        raise ResolveBridgeError(f"réglage {key} illisible : {text!r}")
    if value <= 0:
        raise ResolveBridgeError(f"réglage {key} hors bornes : {value}")
    return value


def timecode_frames(text: Any, rate: int) -> int:
    """HH:MM:SS:FF at a whole rate, as a frame count. Refuses the rest.

    Measured 22 September 2026: Resolve leaves `Frames` empty on an
    audio-only media, so the frame count `speed_facts` needs has to come from
    `Duration` instead. A drop-frame timecode (';' before the frames) is
    refused rather than parsed: it only exists at 29.97 and 59.94, which
    integral_rate already turns away.
    """
    parts = str(text).strip().split(":")
    if len(parts) != 4 or not all(p.isdigit() for p in parts):
        raise ResolveBridgeError(f"timecode illisible : {text!r}")
    hours, minutes, seconds, frames = (int(p) for p in parts)
    if frames >= rate:
        raise ResolveBridgeError(
            f"timecode {text!r} : {frames} images à {rate} i/s")
    return ((hours * 60 + minutes) * 60 + seconds) * rate + frames


def whole_or_zero(value: Any) -> int:
    """A Resolve integer that may not be one. Zero rather than a crash."""
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def speed_facts(item: Any, media: Any) -> dict[str, Any]:
    """The four integers from which the engine derives an exact speed.

    The arithmetic itself stays on the engine side, where it is pure and
    tested without Resolve (see item_speed for the measurement that
    established it, and ResolveImport.cc for the computation). The bridge
    records; it does not conclude.

    `media_rate` goes through integral_rate rather than the reported decimal:
    it divides a frame count on the other side, and 23.976 rounded into 24
    would move the result by a frame per thousand -- enough to read a normal
    clip as retimed. A cadence that is not whole is reported as unmeasurable
    rather than approximated, and the engine refuses that clip by name.
    """
    facts: dict[str, Any] = {}
    for key, name in (("left_offset", "GetLeftOffset"),
                      ("right_offset", "GetRightOffset")):
        getter = getattr(item, name, None)
        if not callable(getter):
            return {"speed_unmeasurable":
                    f"{name} absent de ce build de Resolve"}
        raw = getter()
        if raw is None:
            return {"speed_unmeasurable": f"{name} ne rend rien"}
        try:
            facts[key] = int(raw)
        except (TypeError, ValueError):
            return {"speed_unmeasurable": f"{name} illisible"}
    if media is None:
        return {"speed_unmeasurable": "aucun média derrière le plan"}
    try:
        facts["media_rate"] = integral_rate(media.GetClipProperty("FPS"))
    except ResolveBridgeError as erreur:
        return {"speed_unmeasurable": str(erreur)}
    try:
        facts["media_frames"] = int(media.GetClipProperty("Frames"))
    except (TypeError, ValueError):
        # An audio-only media has no `Frames` at all -- Resolve leaves it
        # empty -- and refusing there rejected every timeline carrying a
        # sound, whatever its picture. Its `Duration` timecode, read at the
        # rate Resolve reports for it, is the same count: measured on four
        # music and effects beds, HH:MM:SS:FF at that rate equals the
        # offsets' total exactly. That rate is the project's rather than the
        # media's, which no audio has, but both sides of the ratio use it, so
        # the speed it yields is still exact.
        try:
            facts["media_frames"] = timecode_frames(
                media.GetClipProperty("Duration"), facts["media_rate"])
        except ResolveBridgeError as erreur:
            return {"speed_unmeasurable":
                    f"ni nombre d'images ni durée lisibles ({erreur})"}
    return facts


def is_transition_item(item: Any) -> bool:
    """Tells a transition apart from a clip, among what a track hands back.

    R10 -- GetItemListInTrack publishes transitions as items of their own,
    named after their kind, and overlapping the clips they join. Measured on
    Resolve 20.3.1, 22 September 2026, on a montage carrying five of them:
    such an item answers None to GetMediaPoolItem, GetSourceStartFrame,
    GetLeftOffset and GetRightOffset, and its GetProperty bag is empty.

    A clip with no file on disk -- a compound clip, a nested timeline, a
    generator -- is *not* one of these: it still has a Media Pool item and
    still publishes the 26 properties, so the engine keeps refusing it by
    name as it always did. Both conditions are required for the same reason:
    a build that answered 0 instead of None would read its transitions as
    clips with no file, which is today's behaviour, not a new silence.
    """
    if item.GetMediaPoolItem() is not None:
        return False
    getter = getattr(item, "GetSourceStartFrame", None)
    if not callable(getter):
        return False
    try:
        return getter() is None
    except (TypeError, ValueError):
        return False


def read_transition(item: Any, origin: int) -> dict[str, Any]:
    """One transition item, as facts. Nothing is judged here either.

    Its bounds are the whole of what Resolve says: the API publishes no
    alignment, and none is inferred here. Where those bounds sit around the
    cut the two clips share is what gives it, and only the engine holds the
    clips -- so it decides, and refuses by name what the document cannot
    express. `name` is Resolve's own name for the kind ("Cross Dissolve"),
    passed on untouched so a refusal can quote it.
    """
    return {
        "name": item.GetName() or "",
        "record_start": int(item.GetStart()) - origin,
        "record_end": int(item.GetEnd()) - origin,
        "duration": whole_or_zero(item.GetDuration()),
    }


def read_item(item: Any, origin: int) -> dict[str, Any]:
    """One timeline item, as facts. Nothing is judged here.

    GetSourceEndFrame is left out for the reason flatten_timeline gives: it
    disagrees with the item duration by one frame on some clips and two on
    others. GetDuration is carried as it comes, and the engine treats it as a
    cross-check because the API does not say whether it counts sequence
    frames or source frames.
    """
    start, end = int(item.GetStart()), int(item.GetEnd())
    media = item.GetMediaPoolItem()
    path = (media.GetClipProperty("File Path") or "").strip() if media else ""
    entry: dict[str, Any] = {
        "name": item.GetName(),
        "record_start": start - origin,
        "record_end": end - origin,
        # Measured on a real montage: a transition item answers None to both
        # of these, and int(None) used to bring the whole read down rather
        # than leaving one refusable item. Zero here is never read as a fact:
        # such an item carries no path, and the engine refuses it by name for
        # that.
        "duration": whole_or_zero(item.GetDuration()),
        "source_start": whole_or_zero(item.GetSourceStartFrame()),
        "fusion_comps": fusion_comp_count(item),
        "path": path,
        "properties": item_properties(item),
    }
    entry.update(speed_facts(item, media))
    if not path:
        entry["no_file_reason"] = (
            (media.GetClipProperty("Type") or "").strip() if media else ""
        ) or "sans fichier"
    return entry


def read_timeline(timeline: Any) -> dict[str, Any]:
    """Maps a Resolve timeline to the manifest the engine reads.

    Video tracks and audio tracks both, with Resolve's own 1-based index
    kept: the document assigns its own, and a refusal has to be able to say
    "V2" to someone looking at Resolve.

    Positions are relative to the timeline's own start frame (90000 at 25 i/s
    for a 01:00:00:00 start), so nothing downstream has to know that
    convention. A gap between two items is left as it is: the document
    addresses positions absolutely, and only a flat render needs contiguity.

    Transitions go in a list of their own rather than among the clips, which
    is what they are: they overlap the clips they join, and mixing them in
    made the engine read that overlap as two clips laid over each other and
    refuse both.
    """
    name = timeline.GetName()
    origin = int(timeline.GetStartFrame())
    tracks: list[dict[str, Any]] = []
    for kind in ("video", "audio"):
        for index in range(1, int(timeline.GetTrackCount(kind) or 0) + 1):
            items = timeline.GetItemListInTrack(kind, index) or []
            clips: list[dict[str, Any]] = []
            transitions: list[dict[str, Any]] = []
            for item in items:
                if is_transition_item(item):
                    transitions.append(read_transition(item, origin))
                else:
                    clips.append(read_item(item, origin))
            tracks.append({
                "kind": kind,
                "index": index,
                "name": track_name(timeline, kind, index),
                "clips": clips,
                "transitions": transitions,
            })
    if not any(track["clips"] for track in tracks):
        raise ResolveBridgeError(f"« {name} » n'a aucun plan.")
    return {
        "name": name,
        "frame_rate": integral_rate(timeline.GetSetting("timelineFrameRate")),
        "origin": origin,
        "width": integral_setting(timeline, "timelineResolutionWidth"),
        "height": integral_setting(timeline, "timelineResolutionHeight"),
        "tracks": tracks,
    }


def read_named_timeline(resolve: Any, name: str) -> dict[str, Any]:
    """The timeline `name` of the open project, as an import manifest."""
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    manifest: dict[str, Any] = {
        "schema": TIMELINE_READ_SCHEMA,
        "project": project.GetName(),
        "resolve_version": resolve.GetVersionString(),
        "unpublished": list(UNPUBLISHED_BY_API),
    }
    manifest.update(read_timeline(find_timeline(project, name)))
    return manifest

RENDER_CODEC = ("mov", "ProRes422LT")
# RESOLVE-2026-09 -- the render exists to give CUTMACHINE something it can
# decode; the finished montage comes back on the BRAW, so the working master's
# resolution changes nothing about the cut. Half of the 2160x3840 timeline
# keeps the framing and a fraction of the encode time. The *rate* is what must
# not move: every frame number in the flattening map is a 25 i/s frame.
RENDER_WIDTH, RENDER_HEIGHT = 1080, 1920


def plan_render(flattened: dict[str, Any], target_dir: str) -> dict[str, Any]:
    """Render settings for one flattened timeline. Pure, so it can be tested.

    SelectAllFrames matters more than it looks: with MarkIn/MarkOut honoured
    instead, a stray in/out left in the timeline would render a fragment, and
    every frame number in the flattening map would point somewhere else.
    """
    return {
        "SelectAllFrames": True,
        "TargetDir": target_dir,
        "CustomName": flattened["name"],
        "UniqueFilenameStyle": 1,  # suffix, never overwrite silently
        "ExportVideo": True,
        "ExportAudio": True,
        "FormatWidth": RENDER_WIDTH,
        "FormatHeight": RENDER_HEIGHT,
    }


def run_renders(resolve: Any, flattening: dict[str, Any],
                target_dir: str, poll=None) -> list[dict[str, Any]]:
    """Queues one ProRes render per flattened timeline and runs only those.

    RESOLVE-2026-09 -- StartRendering() with no argument runs *every* queued
    job. The operator's own queue is none of our business, so this collects
    the ids it created and starts those alone; jobs already waiting stay
    waiting. DeleteAllRenderJobs is never called for the same reason.
    """
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise ResolveBridgeError("Aucun projet ouvert dans Resolve.")
    os.makedirs(target_dir, exist_ok=True)

    if not project.SetCurrentRenderFormatAndCodec(*RENDER_CODEC):
        raise ResolveBridgeError(
            f"Resolve a refusé le format {RENDER_CODEC[0]}/{RENDER_CODEC[1]}.")

    queued = []
    for flattened in flattening["timelines"]:
        timeline = find_timeline(project, flattened["name"])
        if not project.SetCurrentTimeline(timeline):
            raise ResolveBridgeError(
                f"Resolve n'a pas pu activer « {flattened['name']} ».")
        if not project.SetRenderSettings(plan_render(flattened, target_dir)):
            raise ResolveBridgeError(
                f"Réglages de rendu refusés pour « {flattened['name']} ».")
        job = project.AddRenderJob()
        if not job:
            raise ResolveBridgeError(
                f"Resolve n'a pas créé de job pour « {flattened['name']} ».")
        queued.append({"job": job, "name": flattened["name"]})

    if not project.StartRendering([entry["job"] for entry in queued]):
        raise ResolveBridgeError("StartRendering a échoué.")
    if poll:
        poll(project, queued)
    return queued


def find_timeline(project: Any, name: str) -> Any:
    for index in range(1, int(project.GetTimelineCount()) + 1):
        timeline = project.GetTimelineByIndex(index)
        if timeline.GetName() == name:
            return timeline
    raise ResolveBridgeError(f"Timeline introuvable : « {name} ».")


def write_json(payload: dict[str, Any], destination: str,
               summary: str = "") -> int:
    text = json.dumps(payload, ensure_ascii=False, indent=1,
                      sort_keys=True) + "\n"
    if destination == "-":
        sys.stdout.write(text)
    else:
        with open(destination, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"{summary} -> {destination}", file=sys.stderr)
    return 0


def wait_for_renders(project: Any, queued: list[dict[str, Any]]) -> None:
    """Blocks until every queued job leaves the running state.

    Reports per job rather than on IsRenderingInProgress alone: a job that
    fails stops the render without stopping the queue, and a silent partial
    success here would be ingested as if it were the whole interview.
    """
    import time
    pending = {entry["job"]: entry["name"] for entry in queued}
    done: dict[str, str] = {}
    while pending:
        time.sleep(5)
        for job, name in list(pending.items()):
            status = project.GetRenderJobStatus(job) or {}
            state = status.get("JobStatus", "")
            if state in ("Complete", "Failed", "Cancelled"):
                done[job] = state
                del pending[job]
                print(f"  {name} : {state}", file=sys.stderr)
    failed = [job for job, state in done.items() if state != "Complete"]
    if failed:
        raise ResolveBridgeError(
            f"{len(failed)} rendu(s) non terminé(s) : "
            + ", ".join(sorted(done[job] for job in failed)))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Exporte les chutiers du projet Resolve ouvert en "
                    "manifeste d'import CUTMACHINE.")
    parser.add_argument("--output", "-o", default="-",
                        help="Fichier de sortie ('-' pour la sortie standard)")
    parser.add_argument("--send", metavar="TIMELINE.json",
                        help="Renvoie dans Resolve une timeline exportée par "
                             "`cutmachine --export-resolve-timeline`")
    parser.add_argument("--name", metavar="NOM",
                        help="Nom de la timeline créée dans Resolve "
                             "(par défaut celui du montage). Resolve refuse "
                             "un nom déjà pris : sans ça, on ne peut pas "
                             "renvoyer une révision du même montage.")
    parser.add_argument("--srt", metavar="SOUS-TITRES.srt",
                        help="Pose ce SRT sur une piste de sous-titres de la "
                             "timeline renvoyée par --send. Resolve met en "
                             "cache le média importé par son chemin : le pont "
                             "importe une copie nommée d'après son contenu "
                             "(~/Library/Application Support/CUTMACHINE/"
                             "resolve-srt), puis compare les sous-titres posés "
                             "aux cues du fichier et échoue s'ils diffèrent.")
    parser.add_argument("--send-xml", metavar="MONTAGE.xml",
                        help="Renvoie dans Resolve le XML FCP7 écrit par "
                             "`cutmachine --export-resolve-xml` : vitesses "
                             "et fondus enchaînés, que --send ne sait pas "
                             "porter. Le pont y inscrit le timecode de "
                             "départ de chaque rush (Resolve apparie un "
                             "média par ce timecode et par rien d'autre), "
                             "puis relit la timeline posée et échoue si un "
                             "plan a atterri sur un autre rush ou à une "
                             "autre image.")
    parser.add_argument("--flatten", metavar="PRÉFIXE",
                        help="Cartographie les timelines dont le nom commence "
                             "par PRÉFIXE : pour chaque image de sortie, le "
                             "rush et l'image derrière elle.")
    parser.add_argument("--render", metavar="APLATISSEMENT.json",
                        help="Rend en ProRes chaque timeline cartographiée "
                             "par --flatten, et n'exécute que ses propres "
                             "jobs (la file existante n'est pas touchée).")
    parser.add_argument("--target-dir", metavar="DOSSIER",
                        help="Destination des rendus de --render.")
    parser.add_argument("--remap", metavar="MONTAGE.json",
                        help="Réécrit un montage fait sur le rendu à plat "
                             "pour qu'il pointe sur les rushes d'origine. "
                             "Demande --flatten-map et --timeline.")
    parser.add_argument("--flatten-map", metavar="APLATISSEMENT.json",
                        help="Cartographie produite par --flatten.")
    parser.add_argument("--timeline", metavar="NOM",
                        help="Timeline de la cartographie à utiliser pour "
                             "--remap.")
    parser.add_argument("--read-timeline", metavar="NOM",
                        help="Relit la timeline NOM du projet ouvert en "
                             "manifeste pour `cutmachine "
                             "--import-resolve-timeline` : pistes vidéo et "
                             "audio, plans, plages source, transitions. Le "
                             "pont relève ce que Resolve publie ; le moteur "
                             "refuse nommément ce que le document ne sait pas "
                             "porter.")
    arguments = parser.parse_args(argv)

    if arguments.flatten:
        try:
            flattening = read_flattening(connect(), arguments.flatten)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return write_json(flattening, arguments.output, summary=", ".join(
            f"{t['name'].split('_')[-1]}:{len(t['segments'])}"
            for t in flattening["timelines"]))

    if arguments.render:
        if not arguments.target_dir:
            print("--render demande --target-dir.", file=sys.stderr)
            return 1
        try:
            with open(arguments.render, encoding="utf-8") as handle:
                flattening = json.load(handle)
            queued = run_renders(connect(), flattening, arguments.target_dir,
                                 poll=wait_for_renders)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        print(f"{len(queued)} rendu(s) terminé(s) dans "
              f"{arguments.target_dir}", file=sys.stderr)
        return 0

    if arguments.remap:
        if not arguments.flatten_map or not arguments.timeline:
            print("--remap demande --flatten-map et --timeline.",
                  file=sys.stderr)
            return 1
        try:
            with open(arguments.remap, encoding="utf-8") as handle:
                montage = json.load(handle)
            with open(arguments.flatten_map, encoding="utf-8") as handle:
                flattening = json.load(handle)
            wanted = [t for t in flattening["timelines"]
                      if t["name"] == arguments.timeline]
            if not wanted:
                raise ResolveBridgeError(
                    f"« {arguments.timeline} » absente de la cartographie.")
            remapped = remap_to_sources(montage, wanted[0])
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return write_json(remapped, arguments.output,
                          summary=f"{len(remapped['clips'])} plan(s)")

    if arguments.read_timeline:
        try:
            manifest = read_named_timeline(connect(), arguments.read_timeline)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        clips = sum(len(track["clips"]) for track in manifest["tracks"])
        fondus = sum(len(track.get("transitions", ()))
                     for track in manifest["tracks"])
        return write_json(
            manifest, arguments.output,
            summary=f"{manifest['name']} : {clips} plan(s), "
                    f"{len(manifest['tracks'])} piste(s)"
                    + (f", {fondus} transition(s)" if fondus else ""))

    if arguments.send_xml:
        if arguments.srt:
            print("--send-xml ne pose pas de sous-titres : le xmeml n'a pas "
                  "de piste de sous-titres (mesuré : l'export FCP7 de "
                  "Resolve n'en écrit aucune trace) et une timeline importée "
                  "arrive pleine, si bien qu'AppendToTimeline décale chaque "
                  "cue de la longueur du montage. Passe par --send pour un "
                  "montage sans vitesse ni fondu.", file=sys.stderr)
            return 1
        try:
            print(send_timeline_xml(connect(), arguments.send_xml,
                                    arguments.name),
                  file=sys.stderr)
        except (ResolveBridgeError, OSError,
                ElementTree.ParseError) as error:
            print(str(error), file=sys.stderr)
            return 1
        return 0

    if arguments.send:
        try:
            with open(arguments.send, encoding="utf-8") as handle:
                timeline = json.load(handle)
            print(send_timeline(connect(), timeline, arguments.name,
                                arguments.srt),
                  file=sys.stderr)
        except ResolveBridgeError as error:
            print(str(error), file=sys.stderr)
            return 1
        return 0

    try:
        manifest = build_manifest(connect())
    except ResolveBridgeError as error:
        print(str(error), file=sys.stderr)
        return 1
    text = json.dumps(manifest, ensure_ascii=False, indent=1,
                      sort_keys=True) + "\n"
    if arguments.output == "-":
        sys.stdout.write(text)
    else:
        with open(arguments.output, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"{len(manifest['clips'])} rush(es), "
              f"{len(manifest['bins'])} chutier(s) -> {arguments.output}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
