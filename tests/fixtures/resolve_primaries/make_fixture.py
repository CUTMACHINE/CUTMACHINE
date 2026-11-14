"""GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Writes resolve_cubes.txt,
the reference color_effects_tests holds color.resolve_primaries to.

The source is the measurement of 25 October 2026 (GR15, point 2), the
temperature and tint measured on 6 November 2026 (GR16), and the project in
ACEScct measured on 14 November 2026 (GR16), outside the repository: grades written into .drx files, posed on a clip of the probe
project in DaVinci Resolve Studio 21.1.0.17, and the 33-point cube
Resolve's ExportLUT gave for each. No client picture is involved: a cube
depends on the grade only. Each case keeps the stored values it was
written with, as exact fractions, and 125 lattice points (indices 0, 5,
13, 21 and 32 on each axis): input and Resolve's output, clamped to [0, 1]
as the cube is.

  python3 make_fixture.py ~/CUTMACHINE-mesures
"""
import json
import sys
from fractions import Fraction
from pathlib import Path

INDICES = (0, 5, 13, 21, 32)
# The template's neutral (GR12's neutral CDL): Lum Mix stored 0.
TEMPLATE = {"lummix": 0.0}
NAMES = {"sat": "saturation", "lummix": "lum_mix", "contrast": "contrast",
         "pivot": "pivot", "temp": "temperature", "tint": "tint"}
AWARE = {"color_space_aware": 1, "contrast_s_curve": 1}
# The folders of the measurement, under the source: the name each takes in
# the reference, and the project settings it was taken in (DEC46):
# colour-space-aware tools, contrast S-curve.
FOLDERS = {
    "yrgb": ("gr15-facteurs/yrgb", {}),
    "serie2": ("gr15-facteurs/serie2", {}),
    "aware-combi": ("gr15-facteurs/aware-combi", AWARE),
    "contraste-dwg": ("gr15-facteurs/contraste-dwg",
                      {"color_space_aware": 0, "contrast_s_curve": 1}),
    "contraste-lin": ("gr15-facteurs/contraste-lin",
                      {"color_space_aware": 0, "contrast_s_curve": 0}),
    # GR16 -- the temperature and the tint, in the author's setting: alone, then
    # together, then with every other primary (the order in the node), then
    # 24 nodes drawn at random, kept out of the search for the law.
    "temperature": ("gr16-temp-teinte/temp", AWARE),
    "teinte": ("gr16-temp-teinte/tint", AWARE),
    "grille": ("gr16-temp-teinte/grid", AWARE),
    "chaine": ("gr16-temp-teinte/chain", AWARE),
    "hasard": ("gr16-temp-teinte/hasard", AWARE),
    # GR16 -- the same .drx as rcm2-aware and serie2, posed on 6 November
    # 2026 in a project in ACEScct: the primaries as in DaVinci YRGB, the
    # saturation about Rec.709's luma.
    "acescct": ("gr16-temp-teinte/acescct-primaires", {"acescct": 1}),
    # GR16 -- 14 November 2026, the probe project in ACEScct (colour-space-
    # aware tools off, S-curve on, as every ACEScct project of the library):
    # the .drx of the temperature and tint above, posed there, and those of
    # gr15-facteurs/contraste-dwg and contraste-lin; then 24 nodes drawn at
    # random for this measurement, never used to look for the laws.
    "acescct-temperature": ("gr16-acescct/temp", {"acescct": 1}),
    "acescct-teinte": ("gr16-acescct/tint", {"acescct": 1}),
    "acescct-grille": ("gr16-acescct/grid", {"acescct": 1}),
    "acescct-chaine": ("gr16-acescct/chain", {"acescct": 1}),
    "acescct-hasard": ("gr16-acescct/hasard", {"acescct": 1}),
    "acescct-contraste": ("gr16-acescct/contraste",
                          {"acescct": 1, "color_space_aware": 0,
                           "contrast_s_curve": 1}),
    "acescct-contraste-lin": ("gr16-acescct/contraste-lin",
                              {"acescct": 1, "color_space_aware": 0,
                               "contrast_s_curve": 0}),
    "acescct-temoin": ("gr16-acescct/hasard2", {"acescct": 1}),
}
# GR16 -- a matrix needs few settings to be held: every other temperature
# of the sweep (500 apart, and the smallest), every other tint, a grid of
# 6 x 4. The cubes left out stay in the measurement.
KEEP = {
    "temperature": lambda v: (v.get("temp", 0) % 500 == 0 or
                              abs(v.get("temp", 0)) in (10, 100) or
                              v.get("lummix") == 0.0),
    "teinte": lambda v: (v.get("tint", 0) % 20 == 0 or
                         abs(v.get("tint", 0)) == 2 or
                         v.get("lummix") == 0.0),
    "grille": lambda v: (v["temp"] in (-4000, -2500, -1000, 400, 1000, 4000)
                         and v["tint"] in (-100, -20, 50, 100)),
}
# GR16 -- the same choice in ACEScct; of the straight contrast, which gives
# the cubes of DaVinci YRGB to the last digit, the default pivot alone.
KEEP["acescct-temperature"] = KEEP["temperature"]
KEEP["acescct-teinte"] = KEEP["teinte"]
KEEP["acescct-grille"] = KEEP["grille"]
KEEP["acescct-contraste-lin"] = lambda v: "pivot" not in v
for knob in ("lift", "gamma", "gain", "offset"):
    for channel, suffix in (("r", "r"), ("g", "g"), ("b", "b"),
                            ("m", "master")):
        NAMES[f"{knob}.{channel}"] = f"{knob}_{suffix}"


def read_cube(path):
    rows = []
    size = 0
    for line in path.read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "LUT_3D_SIZE":
            size = int(parts[1])
        elif len(parts) == 3 and parts[0][0] in "-.0123456789":
            rows.append(tuple(float(p) for p in parts))
    return size, rows


def fraction(value):
    return Fraction(str(value)).limit_denominator(100000)


def main(source):
    out = [
        "# GR16 -- written by make_fixture.py from Resolve 21.1's cubes of",
        "# 25 October and 6 November 2026. case <name>, param <name>",
        "# <num>/<den>, then point <r> <g> <b> <R> <G> <B>: input and",
        "# Resolve's output.",
    ]
    for folder, (path, settings) in FOLDERS.items():
        cases = json.loads((Path(source) / path / "cases.json").read_text())
        for name, values in cases.items():
            # Past the ends of Resolve's panel: not a stage.
            if (abs(values.get("temp", 0)) > 4000 or
                    abs(values.get("tint", 0)) > 100):
                continue
            if not KEEP.get(folder, lambda v: True)(values):
                continue
            # Past the measured S-curve, or a straight contrast of 2.
            if values.get("contrast", 1) >= (1.8001 if settings.get(
                    "contrast_s_curve", 1) else 2):
                continue
            stored = dict(TEMPLATE)
            stored.update(values)
            size, rows = read_cube(Path(source) / path / f"{name}.cube")
            out.append(f"case {folder}/{name}")
            for key, value in sorted(stored.items()):
                if value == "-":
                    continue  # absent: Resolve's default
                f = fraction(value)
                out.append(f"param {NAMES[key]} {f.numerator}/{f.denominator}")
            for key, value in settings.items():
                out.append(f"param {key} {value}/1")
            for b in INDICES:
                for g in INDICES:
                    for r in INDICES:
                        o = rows[r + size * (g + size * b)]
                        n = size - 1
                        out.append(f"point {r / n:.6g} {g / n:.6g} {b / n:.6g} "
                                   f"{o[0]:.6f} {o[1]:.6f} {o[2]:.6f}")
    target = Path(__file__).with_name("resolve_cubes.txt")
    target.write_text("\n".join(out) + "\n")
    print(target, len(out), "lignes")


if __name__ == "__main__":
    main(sys.argv[1])
