"""MOTION-WEB-2026-10 MW4 -- the imported pieces, ready for the library.

Reads import.mjs's output and writes a folder to drop into the motion
library inbox (MB10): the pieces that stand on their own directly in it,
read on the next listing, and those that lost what makes them (a canvas, a
picture) under a-revoir/, which the library does not read. Every piece's
`origin` says what the import changed; a piece that imitates a brand's
interface says so in its `avoid`.

Usage: python3 -I pack.py <pieces-dir> <classes.json> <out-dir>
(classes.json: the buckets tools/motion-web-measure/analyze.py writes.)
"""
import collections
import json
import os
import re
import shutil
import sys
import tempfile
import uuid
from pathlib import Path

BRANDS = {
    "instagram": "Instagram", "spotify": "Spotify", "tiktok": "TikTok", "x-post": "X",
    "yt-": "YouTube", "youtube": "YouTube", "macos": "macOS", "chatgpt": "ChatGPT",
    "claude": "Claude", "heygen": "HeyGen", "apple": "Apple", "slack": "Slack",
    "notion": "Notion", "figma": "Figma", "linkedin": "LinkedIn", "whatsapp": "WhatsApp",
    "imessage": "iMessage", "discord": "Discord", "google": "Google", "github": "GitHub",
}

NOTE_LABELS = [
    ("transform-origin", "mise à l'échelle et rotation autour du centre au lieu du point d'ancrage CSS (MW5)"),
    ("relevé image par image", "certaines animations relevées image par image (clés denses)"),
    ("bordure non reprise", "bordures CSS absentes"),
    ("bordure inégale sur coins arrondis", "bordures inégales sur coins arrondis tracées en bandes droites"),
    ("dessinée pleine", "bordures doubles ou en relief tracées pleines"),
    ("bordure d'un pseudo-élément texte", "bordure d'un pseudo-élément texte absente"),
    ("textPath", "texte sur tracé SVG absent"),
    ("texte svg sans couleur unie", "texte SVG en dégradé absent"),
    ("texte svg tourné", "texte SVG tourné posé droit"),
    ("ombre non reprise", "ombres non reprises"),
    ("grille CSS", "grille CSS figée aux positions calculées par Chrome"),
    ("pseudo-élément", "pseudo-éléments ::before/::after dans le flux ou au contenu généré absents"),
    ("police absente à l'import", "police absente à l'import : mesures faites dans une police de repli"),
    ("habillage de la démo", "habillage de la démo laissé hors de la pièce"),
    ("formes svg sans tracé", "formes SVG sans tracé (texte, use) absentes"),
    ("svg sans tracé", "SVG sans forme lisible absent"),
    ("ombre de texte multiple", "ombres de texte multiples réduites à la première"),
    ("taille de dégradé radial", "taille de dégradé radial non reprise"),
    ("img non repris", "images absentes"),
    ("canvas non repris", "dessin canvas absent"),
    ("video non repris", "vidéo absente"),
    ("onUpdate", "effets pilotés en JavaScript absents"),
    ("pilotés en JavaScript relevés", "effets pilotés en JavaScript relevés image par image ; texte réécrit en script non suivi"),
    ("relevé image par image trop long", "effets pilotés en JavaScript absents (relevé trop long ou en erreur)"),
    ("groupe svg animé", "groupes SVG animés figés"),
    ("masque svg", "masques SVG en partie repris"),
    ("transform svg non repris", "transformations SVG illisibles non reprises"),
    ("opacité de contour", "opacité de contour non reprise"),
    ("texte à styles mêlés", "textes à styles mêlés : police, taille, graisse ou transformation des passages non reprises"),
    ("passage de texte animé", "animations de passages de texte absentes"),
    ("texte à côté d'éléments", "texte posé à côté de blocs absent"),
    ("motif répété", "motifs de fond répétés absents"),
    ("police de marque", "police de marque remplacée par Helvetica Neue"),
    ("sans équivalent", "propriétés animées sans équivalent dans le format"),
]


def build_pack(pieces_dir, classes_path, out):
    report = json.loads((Path(pieces_dir) / "import-report.json").read_text(encoding="utf-8"))
    classes = {}
    for bucket, names in json.loads(Path(classes_path).read_text(encoding="utf-8")).items():
        for name in names:
            classes[name.split("/")[1]] = bucket.split(" ")[0]
    os.makedirs(os.path.join(out, "a-revoir"))
    shelf = collections.Counter()
    lines = []
    for entry in report:
        if not entry.get("ok"):
            continue
        source = entry["source"]
        name = entry["name"]
        if not isinstance(name, str) or Path(name).name != name or name in (".", ".."):
            raise ValueError("nom de pièce invalide")
        piece = json.loads((Path(pieces_dir) / (name + ".json")).read_text(encoding="utf-8"))
        notes = entry.get("notes", []) + entry.get("refused", [])
        kept = [label for key, label in NOTE_LABELS if any(key in n for n in notes)]
        if entry.get("refused"):
            kept.append(f"{len(entry['refused'])} valeur(s) refusée(s) par le format, retirée(s)")
        painted = classes.get(source) == "C2"
        missing = any(k in label for label in kept for k in ("images absentes", "canvas absent", "vidéo absente"))
        if kept:
            piece["origin"] += " Import gaps: " + "; ".join(kept) + "."
        brand = next((b for k, b in BRANDS.items() if k in source), None)
        if brand:
            piece["avoid"] = (f"Imitates the {brand} interface: check the brand's rules before "
                              "delivering it in a client film.")
        folder = "a-revoir" if painted or missing else ""
        shelf["à revoir" if folder else "prête"] += 1
        with open(os.path.join(out, folder, entry["name"] + ".json"), "w") as f:
            json.dump(piece, f, ensure_ascii=False, indent=2)
            f.write("\n")
        lines.append(f"| {entry['name']} | {'à revoir' if folder else 'prête'} | "
                     f"{entry['stats']['nodes']} | {'; '.join(kept) or '—'} |")
    if not shelf:
        raise ValueError("aucune pièce validée : la bibliothèque précédente est conservée")
    with open(os.path.join(out, "LISEZMOI.md"), "w") as f:
        f.write(
            "# Pièces importées de HyperFrames\n\n"
            "Importées le 11 octobre 2026 par `tools/motion-web-import` (MOTION-WEB-2026-10 MW4), "
            "depuis le registre de HyperFrames (heygen-com/hyperframes, Apache-2.0). Chaque pièce "
            "a été lue et évaluée image par image par le moteur ; aucune n'a encore été rendue ni "
            "regardée sur un Mac.\n\n"
            "Dépose les fichiers `.json` de ce dossier dans le dossier de la bibliothèque : ils "
            "entrent dans la base à la prochaine lecture. `a-revoir/` n'est pas lu : ces pièces ont "
            "perdu ce qui les fait (un dessin canvas, une image).\n\n"
            f"{shelf['prête']} prêtes, {shelf['à revoir']} à revoir.\n\n"
            "| Pièce | État | Nœuds | Écarts à l'import |\n|---|---|---|---|\n" + "\n".join(lines) + "\n")
    return dict(shelf)


def pack(pieces_dir, classes_path, out):
    """FI1 -- ROADMAP.md: publish only a completely constructed pack.

    A failed read or write leaves the previous output intact. A previous
    directory is kept beside it until publication succeeds; if restoring
    it fails, that backup stays on disk for recovery.
    """
    pieces_dir = Path(pieces_dir).resolve()
    classes_path = Path(classes_path).resolve()
    requested = Path(out).absolute()
    if requested.is_symlink():
        raise ValueError("le dossier de sortie ne peut pas être un lien")
    out = requested.resolve()
    if (out == pieces_dir or out in pieces_dir.parents or
            out == classes_path or out in classes_path.parents):
        raise ValueError("la sortie ne peut pas remplacer les sources de l'import")
    if out.exists() and not out.is_dir():
        raise ValueError("la sortie doit être un dossier")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{out.name}.pack-", dir=out.parent) as work:
        staged = Path(work) / "library"
        counts = build_pack(pieces_dir, classes_path, staged)
        previous = out.with_name(f".{out.name}.previous-{uuid.uuid4().hex}")
        had_previous = out.exists()
        if had_previous:
            os.replace(out, previous)
        try:
            os.replace(staged, out)
        except OSError:
            if had_previous:
                os.replace(previous, out)
            raise
        if had_previous:
            shutil.rmtree(previous)
    return counts


if __name__ == "__main__":
    print(pack(*sys.argv[1:4]))
