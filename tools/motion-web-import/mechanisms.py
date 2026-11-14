"""MOTION-WEB-2026-10 MW4 -- the mechanisms the imported pieces share.

Reads the pieces tools/motion-web-import wrote and groups every animated
node by what it does: which properties move, from where to where (signs
and magnitudes rounded), with which curve and over how long. A mechanism
that recurs across many pieces is a candidate generic piece; they are
ranked by the number of piece families (a name without its trailing
number) that use them; tracks sampled frame by frame are left out.

Usage: python3 -I mechanisms.py <pieces-dir> [top]
"""
import collections
import glob
import json
import os
import re
import sys


def frames(value):
    if isinstance(value, str) and "/" in value:
        n, d = value.split("/")
        return int(n) / int(d)
    return float(value)


def bucket(prop, a, b):
    """A move, named coarsely: opacity 0->1, translateY +40->0, scale 0.8->1."""
    if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
        return f"{prop} change"
    if prop == "opacity":
        return f"opacity {round(a, 1):g}->{round(b, 1):g}"
    if prop.startswith("scale"):
        return f"{prop} {round(a, 1):g}->{round(b, 1):g}"
    delta = b - a
    if abs(delta) < 1e-6:
        return None
    size = min((4, 12, 32, 80, 200, 600, 2000), key=lambda s: abs(abs(delta) - s))
    return f"{prop} {'+' if delta > 0 else '-'}{size}"


def main():
    directory = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    signatures = collections.Counter()
    pieces_with = collections.defaultdict(set)
    for path in glob.glob(os.path.join(directory, "*.json")):
        if path.endswith("import-report.json"):
            continue
        piece = json.load(open(path))
        fps = piece["composition"]["fps"]
        # Variants of one piece (carousel-orbit-1 to -5) are one family.
        name = re.sub(r"-\d+$", "", os.path.basename(path)[:-5])

        def visit(node):
            animate = node.get("animate", {})
            moves = []
            curve = set()
            span = set()
            for prop, spec in animate.items():
                keys = spec.get("interpolate")
                if not keys or len(keys["frames"]) < 2:
                    continue
                if len(keys["frames"]) > 6:
                    # Sampled frame by frame: not an author's gesture.
                    continue
                values = keys["values"]
                at = [frames(f) for f in keys["frames"]]
                # The first move of the property: its entrance, usually.
                move = bucket(prop, values[0], values[1])
                if move:
                    moves.append(move)
                    easing = keys.get("easing", "linear")
                    curve.add(easing if isinstance(easing, str) else easing[0])
                    span.add(round((at[1] - at[0]) / fps * 20) / 20)
            if moves and len(curve) == 1 and len(span) == 1:
                signature = f"{' + '.join(sorted(moves))} | {curve.pop()} | {span.pop():g} s"
                signatures[signature] += 1
                pieces_with[signature].add(name)
            for child in node.get("children", []):
                visit(child)

        visit(piece["composition"]["root"])
    ranked = sorted(signatures, key=lambda k: (-len(pieces_with[k]), -signatures[k]))
    for signature in ranked[:top]:
        count = signatures[signature]
        print(f"{count:4d} nodes in {len(pieces_with[signature]):3d} families  {signature}")


main()
