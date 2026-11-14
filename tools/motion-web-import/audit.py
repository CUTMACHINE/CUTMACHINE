"""MOTION-WEB-2026-10 MW4 -- what the second Mac comparison asked to count,
over a folder of pieces (the ready shelf of pack.py, or import.mjs's
output with its report beside it).

1. text nodes given a fixed width, and boxes placed absolutely by a grid
   (left/top/width/height together), the frozen-layout signature;
3. pieces whose report notes a repeated background left out;
4. roots painted by a demo's mount card: a translucent background, a
   filter or rounded corners on the composition root.

And what the third asked: pieces whose drawn paths stay still (a dash
tween left without equivalent) against pieces with a trim animated, mixed
texts that lose something of their spans, pseudo-elements left out.

And the fourth: pieces drawn through an SVG mask, pieces whose script
effects are read frame by frame or left out, SVG groups left still.

Usage: python3 -I audit.py <pieces-dir> [import-report.json]
"""
import glob
import json
import os
import re
import sys


def main():
    directory = sys.argv[1]
    report_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(directory, "import-report.json")
    names = {os.path.basename(p)[:-5] for p in glob.glob(os.path.join(directory, "*.json"))
             if not p.endswith("import-report.json")}
    text_widths = 0
    text_width_pieces = set()
    frozen = 0
    frozen_pieces = set()
    painted_roots = []
    trimmed = set()
    masked = set()
    for name in sorted(names):
        piece = json.load(open(os.path.join(directory, name + ".json")))
        root = piece["composition"]["root"]

        def visit(node):
            nonlocal text_widths, frozen
            style = node.get("style", {})
            if node.get("type") == "path" and any(k in node.get("animate", {}) for k in ("trimStart", "trimEnd", "strokeDashoffset")):
                trimmed.add(name)
            if style.get("mixBlendMode") == "mask-in":
                masked.add(name)
            if node.get("type") == "text" and "width" in style:
                text_widths += 1
                text_width_pieces.add(name)
            if style.get("position") == "absolute" and all(k in style for k in ("left", "top", "width", "height")) \
                    and not (style.get("left") == 0 and style.get("top") == 0):
                frozen += 1
                frozen_pieces.add(name)
            for child in node.get("children", []):
                visit(child)

        visit(root)
        style = root.get("style", {})
        bg = style.get("backgroundColor", "")
        if (isinstance(bg, str) and bg.startswith("rgba(")) or "filter" in style or "borderRadius" in style:
            painted_roots.append(name)
    repeated = 0
    still, mixed, pseudo = set(), set(), set()
    scripted, unscripted, groups = set(), set(), set()
    if os.path.exists(report_path):
        for entry in json.load(open(report_path)):
            if entry["name"] not in names:
                continue
            notes = entry.get("notes", [])
            if any("motif répété" in n for n in notes):
                repeated += 1
            if any(re.search(r"animation\(s\) de « [^»]*dash", n, re.I) for n in notes):
                still.add(entry["name"])
            if any("texte à styles mêlés" in n or "passage de texte animé" in n for n in notes):
                mixed.add(entry["name"])
            if any("pseudo-élément" in n for n in notes):
                pseudo.add(entry["name"])
            if any("pilotés en JavaScript relevés" in n for n in notes):
                scripted.add(entry["name"])
            if any("onUpdate" in n or "relevé image par image trop long" in n for n in notes):
                unscripted.add(entry["name"])
            if any("groupe svg animé" in n for n in notes):
                groups.add(entry["name"])
    print(f"pièces lues : {len(names)}")
    print(f"1. textes à largeur fixe : {text_widths} dans {len(text_width_pieces)} pièces")
    print(f"1. boîtes figées par une grille : {frozen} dans {len(frozen_pieces)} pièces")
    print(f"3. pièces avec un fond répété retiré : {repeated}")
    print(f"4. racines peintes par l'habillage d'une démo : {len(painted_roots)}")
    print(f"tracés : pièces dont un tracé animé reste figé : {len(still)} ; pièces au tracé animé : {len(trimmed)}")
    print(f"textes à styles mêlés : pièces qui perdent quelque chose de leurs passages : {len(mixed)}")
    print(f"pseudo-éléments : pièces qui en laissent hors : {len(pseudo)}")
    print(f"masques svg : pièces dessinées à travers un masque : {len(masked)}")
    print(f"effets pilotés en script : relevés image par image : {len(scripted)} ; laissés : {len(unscripted)}")
    print(f"groupes svg animés laissés figés : {len(groups)}")


main()
