#!/usr/bin/env python3
"""GR12 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Measures, from the outside,
what DaVinci Resolve does with a CDL.

Resolve does not publish its grading maths, and its licence forbids
disassembling it. What it does publish is a way to watch the result:
TimelineItem.ExportLUT samples the whole grade of a clip into a .cube. A
known CDL set with SetCDL, then the cube exported, turns every lattice point
into a measurement -- no render, no codec, no image in between -- so the
comparison holds to the precision Resolve writes its numbers with.

The cases are chosen to tell apart what the manual leaves open (p. 3475 gives
only "(Input * Slope + Offset)^Power"): what happens below zero before the
power, whether anything clips at 1, which luma weights the saturation uses,
and whether the exported cube carries the project's colour management. The
engine's own reading is ColorCdl.h: negatives pass the power unchanged, no
clamp, Rec.709 weights.

Split like sidecar/resolve_bridge.py: the cube reader, the candidate formulas
and the comparison are pure, and the sweep runs on duck-typed Resolve
objects, both tested without Resolve (tests/resolve_grade_probe_tests.py).
Only `connect` needs Resolve Studio running.

Every call into Resolve runs under a watchdog that ends the process when the
call does not return: on 29 September 2026 a script froze Resolve 21 behind
an invisible dialog, and the probe would otherwise hang with it. The probe
opens or creates its project only when told to (`setup --create-project`),
which is meant for a Resolve with no unsaved work open -- leaving a modified
project is how a dialog lands over someone's work -- and every command
refuses to run outside the measurement project.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import os
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterator


DEFAULT_PROJECT = "CUTMACHINE sonde étalonnage"
DEFAULT_TIMELINE = "Sonde étalonnage"
DEFAULT_TIMEOUT = 30.0
# A 10-bit code value is 1/1023; this is a fifth of one. Resolve writes six
# decimals and grades in 32-bit float, both far below it.
DEFAULT_TOLERANCE = 2e-4
LUT_SIZES = (17, 33, 65)
RECORDS = "records.jsonl"
JOURNAL = "journal.txt"
REPORT = "report.json"


class ProbeError(RuntimeError):
    """Raised with a message meant to be shown to the user, in French."""


# --- CDL ---------------------------------------------------------------------

Triple = tuple  # three floats, r g b


def shortest(value: float) -> str:
    """The shortest decimal that reads back as `value`, as the engine's
    ResolveSetCdlMap writes it ("1", not "1.0")."""
    text = repr(float(value))
    return text[:-2] if text.endswith(".0") else text


@dataclass(frozen=True)
class Cdl:
    slope: Triple = (1.0, 1.0, 1.0)
    offset: Triple = (0.0, 0.0, 0.0)
    power: Triple = (1.0, 1.0, 1.0)
    saturation: float = 1.0

    def resolve_map(self, node: int = 1) -> dict[str, str]:
        """The map TimelineItem.SetCDL takes: strings, "r g b" per knob."""
        def triple(values: Triple) -> str:
            return " ".join(shortest(value) for value in values)
        return {
            "NodeIndex": str(node),
            "Slope": triple(self.slope),
            "Offset": triple(self.offset),
            "Power": triple(self.power),
            "Saturation": shortest(self.saturation),
        }

    def as_json(self) -> dict[str, Any]:
        return {
            "slope": list(self.slope),
            "offset": list(self.offset),
            "power": list(self.power),
            "saturation": self.saturation,
        }

    @staticmethod
    def from_json(value: dict[str, Any]) -> Cdl:
        return Cdl(tuple(float(v) for v in value["slope"]),
                   tuple(float(v) for v in value["offset"]),
                   tuple(float(v) for v in value["power"]),
                   float(value.get("saturation", 1.0)))

    @staticmethod
    def from_resolve_map(value: dict[str, Any]) -> Cdl:
        def triple(key: str) -> Triple:
            numbers = str(value[key]).split()
            if len(numbers) != 3:
                raise ProbeError(f"{key} : trois nombres attendus, lu « "
                                 f"{value[key]} ».")
            return tuple(float(number) for number in numbers)
        return Cdl(triple("Slope"), triple("Offset"), triple("Power"),
                   float(value.get("Saturation", "1")))


@dataclass(frozen=True)
class CdlCase:
    name: str
    cdl: Cdl
    question: str


NEUTRAL = Cdl()

# Each case answers one question; tests/resolve_grade_probe_tests.py checks
# that, together, they tell every pair of candidate formulas apart.
CDL_CASES = (
    CdlCase("neutre", NEUTRAL,
            "ce que le LUT exporté contient en plus de la CDL"),
    CdlCase("pente", Cdl(slope=(1.25, 1.0, 0.8)),
            "si une valeur au-dessus de 1 est écrêtée"),
    CdlCase("decalage_negatif", Cdl(offset=(-0.1, -0.1, -0.1)),
            "si une valeur sous 0 est écrêtée, sans puissance"),
    CdlCase("decalage_colore", Cdl(offset=(0.05, 0.0, -0.05)),
            "un décalage par canal, les lumières de tirage"),
    CdlCase("puissance", Cdl(power=(1.4, 1.0, 0.7)),
            "la puissance seule, entre 0 et 1"),
    CdlCase("negatif_puissance",
            Cdl(offset=(-0.1, -0.1, -0.1), power=(1.5, 1.5, 1.5)),
            "la puissance d'une valeur négative : laissée, mise à 0 ou autre"),
    CdlCase("negatif_saturation",
            Cdl(offset=(-0.2, 0.0, 0.0), saturation=1.5),
            "une valeur négative dans la luma de la saturation"),
    CdlCase("saturation_0", Cdl(saturation=0.0),
            "les poids de la luma, lus directement"),
    CdlCase("saturation_05", Cdl(saturation=0.5),
            "une désaturation"),
    CdlCase("saturation_15", Cdl(saturation=1.5),
            "une saturation qui sort de 0–1"),
    CdlCase("depassement_saturation",
            Cdl(slope=(1.3, 1.0, 1.0), power=(0.8, 1.0, 1.0),
                saturation=0.5),
            "une valeur au-dessus de 1 avant la puissance, vue par la luma"),
    CdlCase("combinee",
            Cdl((1.05, 1.0, 0.95), (0.01, 0.0, -0.01), (0.95, 1.0, 1.05), 1.1),
            "un étalonnage ordinaire, tout ensemble"),
    CdlCase("forte", Cdl((2.0, 2.0, 2.0), (-0.2, -0.2, -0.2), (0.8, 0.8, 0.8)),
            "une correction forte, des deux côtés de 0–1"),
)


def cases_from_export(reply: dict[str, Any]) -> list[CdlCase]:
    """The CDLs of an export_cdl reply (MCP or --export-cdl), applied with the
    very map the engine wrote for SetCDL, so the probe checks that map too."""
    cases = []
    for index, clip in enumerate(reply.get("clips", []), start=1):
        mapped = Cdl.from_resolve_map(clip["resolve_set_cdl"])
        if "cdl" in clip and Cdl.from_json(clip["cdl"]) != mapped:
            raise ProbeError(
                f"Plan {clip.get('clip_id', index)} : la table resolve_set_cdl "
                "ne redonne pas la CDL de la réponse.")
        label = clip.get("media") or clip.get("clip_id") or str(index)
        cases.append(CdlCase(f"plan_{index:02d}", mapped,
                             f"un plan de CUTMACHINE ({label}), espace "
                             f"{reply.get('space', '?')}"))
    if not cases:
        raise ProbeError("La réponse d'export_cdl ne contient aucun plan.")
    return cases


# --- Candidate formulas ------------------------------------------------------

LUMA_WEIGHTS = {
    "rec709": (0.2126, 0.7152, 0.0722),
    "rec601": (0.299, 0.587, 0.114),
}
# What happens to x * slope + offset before the power: "pass" leaves a value
# at or below zero unchanged (OpenColorIO's no-clamp style, the engine's),
# "zero" raises it to 0, "unit" clamps to [0, 1] (the ASC's v1.2 style).
BASE_MODES = ("pass", "zero", "unit")
# After the saturation: nothing, or a clamp to [0, 1].
OUTPUT_MODES = ("none", "unit")


@dataclass(frozen=True)
class Variant:
    base: str
    weights: str
    output: str
    # Luma weights not in LUMA_WEIGHTS: the ones measured on a cube.
    values: Triple | None = None

    @property
    def name(self) -> str:
        return f"{self.base}/{self.weights}/{self.output}"


VARIANTS = tuple(Variant(base, weights, output)
                 for base in BASE_MODES
                 for weights in LUMA_WEIGHTS
                 for output in OUTPUT_MODES)
# ColorCdl.h.
ENGINE_VARIANT = Variant("pass", "rec709", "none")
# What a cube can show of it. The cubes Resolve writes hold no output outside
# [0, 1], in ACEScct too (10 October 2026), whether the clip or the file
# clamps -- a lattice cannot tell. So the engine is judged clamped the same
# way, which is also what its display-referred export renders
# (BuildColorGradeCube).
ENGINE_IN_CUBE = Variant("pass", "rec709", "unit")


def apply_cdl(cdl: Cdl, rgb: Triple, variant: Variant) -> Triple:
    graded = []
    for channel in range(3):
        value = rgb[channel] * cdl.slope[channel] + cdl.offset[channel]
        if variant.base == "unit":
            value = min(max(value, 0.0), 1.0)
        elif variant.base == "zero":
            value = max(value, 0.0)
        if value > 0.0:
            value = value ** cdl.power[channel]
        graded.append(value)
    red, green, blue = variant.values or LUMA_WEIGHTS[variant.weights]
    luma = red * graded[0] + green * graded[1] + blue * graded[2]
    graded = [luma + cdl.saturation * (value - luma) for value in graded]
    if variant.output == "unit":
        graded = [min(max(value, 0.0), 1.0) for value in graded]
    return tuple(graded)


# --- Cubes -------------------------------------------------------------------

@dataclass
class Cube:
    size: int
    domain_min: Triple
    domain_max: Triple
    # Red moves fastest, then green, then blue, as .cube stores it.
    table: list

    def input_at(self, red: int, green: int, blue: int) -> Triple:
        indices = (red, green, blue)
        return tuple(
            self.domain_min[axis] + (self.domain_max[axis] -
                                     self.domain_min[axis]) *
            indices[axis] / (self.size - 1)
            for axis in range(3))

    def output_at(self, red: int, green: int, blue: int) -> Triple:
        return self.table[red + self.size * (green + self.size * blue)]

    def lattice(self) -> Iterator[tuple[Triple, Triple]]:
        for blue in range(self.size):
            for green in range(self.size):
                for red in range(self.size):
                    yield (self.input_at(red, green, blue),
                           self.output_at(red, green, blue))


def parse_cube(text: str, source: str = "LUT") -> Cube:
    size = 0
    domain_min: Triple = (0.0, 0.0, 0.0)
    domain_max: Triple = (1.0, 1.0, 1.0)
    table = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        key = words[0].upper()
        if key == "TITLE":
            continue
        if key == "LUT_1D_SIZE":
            raise ProbeError(f"{source} : LUT 1D ou 1D+3D, non pris en "
                             "charge par la sonde.")
        try:
            if key == "LUT_3D_SIZE":
                size = int(words[1])
            elif key == "DOMAIN_MIN":
                domain_min = tuple(float(word) for word in words[1:4])
            elif key == "DOMAIN_MAX":
                domain_max = tuple(float(word) for word in words[1:4])
            elif key == "LUT_3D_INPUT_RANGE":
                low, high = float(words[1]), float(words[2])
                domain_min, domain_max = (low,) * 3, (high,) * 3
            elif key[0].isalpha():
                raise ProbeError(f"{source}, ligne {number} : mot-clé inconnu "
                                 f"« {words[0]} ».")
            else:
                values = tuple(float(word) for word in words)
                if len(values) != 3:
                    raise ValueError
                table.append(values)
        except (ValueError, IndexError):
            raise ProbeError(f"{source}, ligne {number} illisible : "
                             f"« {raw.strip()} ».") from None
    if size < 2:
        raise ProbeError(f"{source} : LUT_3D_SIZE absent ou trop petit.")
    if len(table) != size ** 3:
        raise ProbeError(f"{source} : {len(table)} valeurs pour une taille "
                         f"{size} ({size ** 3} attendues).")
    return Cube(size, domain_min, domain_max, table)


def read_cube(path: Path) -> Cube:
    return parse_cube(Path(path).read_text(encoding="utf-8"), str(path))


def cube_text(size: int, transform: Callable[[Triple], Triple],
              decimals: int = 6) -> str:
    """A .cube of `transform` on [0, 1], six decimals as Resolve writes."""
    lines = [f"LUT_3D_SIZE {size}"]
    for blue in range(size):
        for green in range(size):
            for red in range(size):
                rgb = (red / (size - 1), green / (size - 1), blue / (size - 1))
                lines.append(" ".join(f"{value:.{decimals}f}"
                                      for value in transform(rgb)))
    return "\n".join(lines) + "\n"


# --- Comparison --------------------------------------------------------------

@dataclass
class Deviation:
    max_abs: float
    mean_abs: float
    worst_input: Triple
    worst_expected: Triple
    worst_measured: Triple

    def as_json(self) -> dict[str, Any]:
        return {
            "max_abs": self.max_abs,
            "max_10bit_codes": self.max_abs * 1023,
            "mean_abs": self.mean_abs,
            "worst_input": list(self.worst_input),
            "worst_expected": list(self.worst_expected),
            "worst_measured": list(self.worst_measured),
        }


def deviation(cube: Cube, expected: Callable[[Triple], Triple]) -> Deviation:
    worst = -1.0
    total = 0.0
    count = 0
    found = None
    for rgb, measured in cube.lattice():
        wanted = expected(rgb)
        for channel in range(3):
            error = abs(measured[channel] - wanted[channel])
            total += error
            count += 1
            if error > worst:
                worst = error
                found = (rgb, wanted, measured)
    return Deviation(worst, total / count, *found)


def measured_luma_weights(cube: Cube) -> Triple | None:
    """The saturation's luma weights, read on the cube of a saturation-0
    CDL: each primary comes out as a grey of its own weight. None when the
    primaries do not come out grey."""
    weights = []
    for axis in range(3):
        index = [0, 0, 0]
        index[axis] = cube.size - 1
        out = cube.output_at(*index)
        if max(out) - min(out) > 1e-3:
            return None
        weights.append(sum(out) / 3)
    return tuple(weights)


def measured_variants(weights: Triple) -> tuple[Variant, ...]:
    return tuple(Variant(base, "mesure", output, weights)
                 for base in BASE_MODES for output in OUTPUT_MODES)


def compare_cdl(cube: Cube, cdl: Cdl, variants: tuple[Variant, ...] = VARIANTS
                ) -> list[tuple[Variant, Deviation]]:
    """Every candidate formula against the cube, closest first."""
    results = [(variant,
                deviation(cube, lambda rgb, v=variant: apply_cdl(cdl, rgb, v)))
               for variant in variants]
    results.sort(key=lambda pair: (pair[1].max_abs, pair[1].mean_abs))
    return results


def describe(cube: Cube) -> dict[str, Any]:
    """For a grade set by hand: how far from identity, the response along
    the grey axis, and whether each channel depends on itself only (wheels
    without saturation do; anything mixing through a luma does not)."""
    size = cube.size
    step = max(1, (size - 1) // 8)
    grey = []
    for index in list(range(0, size, step)) + ([size - 1] if
                                               (size - 1) % step else []):
        grey.append({"input": cube.input_at(index, index, index)[0],
                     "output": list(cube.output_at(index, index, index))})
    spread = [0.0, 0.0, 0.0]
    for channel in range(3):
        for own in range(size):
            values = []
            for first in range(size):
                for second in range(size):
                    index = [first, second]
                    index.insert(channel, own)
                    values.append(cube.output_at(*index)[channel])
            spread[channel] = max(spread[channel], max(values) - min(values))
    return {
        "identity": deviation(cube, lambda rgb: rgb).as_json(),
        "grey_axis": grey,
        "channel_spread": spread,
    }


# --- Talking to Resolve ------------------------------------------------------

@contextlib.contextmanager
def guarded(what: str, seconds: float,
            leave: Callable[[int], Any] = os._exit) -> Iterator[None]:
    """Ends the process when the block outlives `seconds`: a call blocked in
    Resolve never comes back to Python, so a signal handler would wait for
    it too. A thread does not."""
    def fire() -> None:
        sys.stderr.write(
            f"\nResolve ne répond plus après {seconds:g} s, sur {what}.\n"
            "Une fenêtre attend peut-être dans Resolve. Ne relance pas la "
            "sonde : regarde l'écran de Resolve d'abord.\n")
        sys.stderr.flush()
        leave(124)
    timer = threading.Timer(seconds, fire)
    timer.daemon = True
    timer.start()
    try:
        yield
    finally:
        timer.cancel()


class Session:
    """Calls into Resolve, each under the watchdog and written to the journal
    first, so a killed run says which call it died on."""

    def __init__(self, resolve: Any, out_dir: Path | None,
                 timeout: float = DEFAULT_TIMEOUT,
                 leave: Callable[[int], Any] = os._exit,
                 echo: Callable[[str], Any] | None = None):
        self.resolve = resolve
        self.out_dir = out_dir
        self.timeout = timeout
        self.leave = leave
        self.echo = echo or (lambda text: print(text, flush=True))

    def note(self, text: str) -> None:
        if self.out_dir is not None:
            with open(self.out_dir / JOURNAL, "a", encoding="utf-8") as file:
                file.write(f"{datetime.datetime.now().isoformat()} {text}\n")

    def call(self, what: str, function: Callable[..., Any], *args: Any) -> Any:
        self.note(f"-> {what}")
        with guarded(what, self.timeout, self.leave):
            result = function(*args)
        self.note(f"<- {what}")
        return result

    def record(self, entry: dict[str, Any]) -> None:
        with open(self.out_dir / RECORDS, "a", encoding="utf-8") as file:
            file.write(json.dumps(entry, ensure_ascii=False) + "\n")


COLOR_SETTINGS = (
    "colorScienceMode", "isAutoColorManage", "rcmPresetMode",
    "separateColorSpaceAndGamma", "colorSpaceTimeline",
    "colorSpaceTimelineGamma", "colorSpaceInput", "colorSpaceInputGamma",
    "colorSpaceOutput", "colorSpaceOutputGamma", "timelineWorkingLuminanceMode",
    "acesVersion",
)


def measurement_project(session: Session, project_name: str) -> Any:
    manager = session.call("GetProjectManager",
                           session.resolve.GetProjectManager)
    project = session.call("GetCurrentProject", manager.GetCurrentProject)
    if project is None:
        raise ProbeError("Aucun projet ouvert dans Resolve.")
    name = session.call("Project.GetName", project.GetName)
    if name != project_name:
        raise ProbeError(
            f"Le projet ouvert est « {name} ». La sonde ne travaille que dans "
            f"« {project_name} ». Crée-le ou ouvre-le, ou lance `setup "
            "--create-project` quand aucun travail non enregistré n'est "
            "ouvert dans Resolve, puis relance.")
    return project


def open_measurement_project(session: Session, project_name: str) -> str:
    """Loads the measurement project, or creates it. Only for a Resolve with
    no unsaved work open: leaving a modified project may ask whether to save
    it, in a dialog no one may be there to answer."""
    manager = session.call("GetProjectManager",
                           session.resolve.GetProjectManager)
    current = session.call("GetCurrentProject", manager.GetCurrentProject)
    if (current is not None and
            session.call("Project.GetName", current.GetName) == project_name):
        return f"Projet « {project_name} » déjà ouvert."
    names = session.call("GetProjectListInCurrentFolder",
                         manager.GetProjectListInCurrentFolder) or []
    if project_name in names:
        opened = session.call("LoadProject", manager.LoadProject, project_name)
        done = "ouvert"
    else:
        opened = session.call("CreateProject", manager.CreateProject,
                              project_name)
        done = "créé"
    if opened is None:
        raise ProbeError(f"Projet « {project_name} » : Resolve ne l'a pas "
                         f"{done}.")
    return f"Projet « {project_name} » {done}."


COLOR_SCIENCES = ("davinciYRGB", "davinciYRGBColorManaged",
                  "davinciYRGBColorManagedv2", "acescc", "acescct")


def set_color_science(session: Session, project: Any, mode: str) -> str:
    if mode not in COLOR_SCIENCES:
        raise ProbeError(f"Science couleur inconnue : {mode}.")
    done = session.call(f"SetSettings(colorScienceMode={mode})",
                        project.SetSettings, {"colorScienceMode": mode})
    now = (session.call("GetSettings", project.GetSettings) or {}).get(
        "colorScienceMode")
    if not done or now != mode:
        raise ProbeError(f"Resolve n'a pas pris la science couleur {mode} "
                         f"(lu : {now}).")
    return f"Science couleur : {mode}."


def measurement_item(session: Session, project_name: str) -> tuple[Any, Any]:
    project = measurement_project(session, project_name)
    timeline = session.call("GetCurrentTimeline", project.GetCurrentTimeline)
    if timeline is None:
        raise ProbeError("Le projet de mesure n'a pas de timeline ouverte : "
                         "lance d'abord `setup --media <fichier>`.")
    items = session.call("GetItemListInTrack", timeline.GetItemListInTrack,
                         "video", 1) or []
    if not items:
        raise ProbeError("La piste V1 de la timeline de mesure est vide : "
                         "lance d'abord `setup --media <fichier>`.")
    return project, items[0]


def project_facts(session: Session, project: Any) -> dict[str, Any]:
    resolve = session.resolve
    facts = {
        "resolve": session.call("GetVersionString", resolve.GetVersionString),
        "product": session.call("GetProductName", resolve.GetProductName),
        "project": session.call("Project.GetName", project.GetName),
    }
    # GetSetting(key) is deprecated in 21.1; GetSettings gives them all.
    settings = session.call("GetSettings", project.GetSettings) or {}
    facts["settings"] = {key: settings.get(key) for key in COLOR_SETTINGS}
    return facts


def lut_export_type(resolve: Any, size: int) -> Any:
    if size not in LUT_SIZES:
        raise ProbeError(f"Taille de LUT {size} : Resolve n'exporte qu'en "
                         "17, 33 ou 65 points.")
    return getattr(resolve, f"EXPORT_LUT_{size}PTCUBE")


def read_back_cdl(session: Session, item: Any) -> dict[str, Any]:
    """apply_cdl and GRADE-2026-09 say Resolve's GetCDL returns the strings
    SetCDL takes; the 21.1 stubs declare no GetCDL. Record what is there,
    with a node number as SetCDL's map has one, then without: its signature
    is not known either."""
    method = getattr(item, "GetCDL", None)
    if method is None:
        return {"available": False}
    attempts = []
    for args in ((1,), ()):
        try:
            value = session.call(f"GetCDL{args}", method, *args)
        except Exception as error:  # the proxy may raise for an unknown method
            attempts.append(repr(error))
            continue
        if value is None:
            attempts.append(None)
            continue
        if isinstance(value, dict):
            value = {str(key): str(entry) for key, entry in value.items()}
        return {"available": True, "args": list(args), "value": value}
    return {"available": False, "attempts": attempts}


def on_color_page(session: Session) -> None:
    """ExportLUT answers False and writes nothing from any page but Color
    (Resolve 21.1.0.17, 10 October 2026; neither the stubs nor the README
    say so), as Generate LUT is a Color page command."""
    page = session.call("GetCurrentPage", session.resolve.GetCurrentPage)
    if page != "color" and not session.call(
            "OpenPage(color)", session.resolve.OpenPage, "color"):
        raise ProbeError("Resolve n'a pas ouvert la page Color, où seul "
                         "l'export de LUT fonctionne.")


def export_lut(session: Session, item: Any, export_type: Any, path: Path,
               what: str, wait: float = 5.0) -> bool:
    if path.exists():
        path.unlink()
    done = session.call(f"ExportLUT ({what})", item.ExportLUT, export_type,
                        str(path))
    deadline = time.monotonic() + wait
    while done and not path.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    return bool(done) and path.exists()


def session_header(session: Session, project: Any, command: str,
                   lut_size: int) -> dict[str, Any]:
    header = {"kind": "session", "command": command, "lut_size": lut_size,
              "started": datetime.datetime.now().isoformat(timespec="seconds")}
    header.update(project_facts(session, project))
    return header


def run_cdl_sweep(session: Session, project_name: str, cases: list[CdlCase],
                  lut_size: int) -> list[dict[str, Any]]:
    project, item = measurement_item(session, project_name)
    export_type = lut_export_type(session.resolve, lut_size)
    on_color_page(session)
    session.record(session_header(session, project, "cdl", lut_size))
    records = []
    try:
        for case in cases:
            session.echo(f"{case.name} : {case.question}")
            entry = {"kind": "cdl", "case": case.name,
                     "question": case.question, "cdl": case.cdl.as_json(),
                     "resolve_set_cdl": case.cdl.resolve_map()}
            entry["set_cdl"] = bool(session.call(
                f"SetCDL ({case.name})", item.SetCDL, case.cdl.resolve_map()))
            if entry["set_cdl"]:
                entry["get_cdl"] = read_back_cdl(session, item)
                lut = session.out_dir / f"{case.name}.cube"
                entry["exported"] = export_lut(session, item, export_type, lut,
                                               case.name)
                entry["lut"] = lut.name
            session.record(entry)
            records.append(entry)
    finally:
        session.call("SetCDL (neutre, pour finir)", item.SetCDL,
                     NEUTRAL.resolve_map())
    return records


def run_manual_export(session: Session, project_name: str, label: str,
                      note: str, lut_size: int) -> dict[str, Any]:
    project, item = measurement_item(session, project_name)
    export_type = lut_export_type(session.resolve, lut_size)
    on_color_page(session)
    session.record(session_header(session, project, "export", lut_size))
    safe = "".join(c if c.isalnum() or c in "+-._" else "_" for c in label)
    lut = session.out_dir / f"main_{safe}.cube"
    entry = {"kind": "manual", "label": label, "note": note, "lut": lut.name,
             "exported": export_lut(session, item, export_type, lut, label)}
    session.record(entry)
    return entry


def run_setup(session: Session, project_name: str, media: Path | None,
              create_project: bool = False, color_science: str | None = None,
              timeline_name: str = DEFAULT_TIMELINE) -> str:
    said = []
    if create_project:
        said.append(open_measurement_project(session, project_name))
    project = measurement_project(session, project_name)
    if color_science:
        said.append(set_color_science(session, project, color_science))
    timeline = session.call("GetCurrentTimeline", project.GetCurrentTimeline)
    if timeline is not None:
        items = session.call("GetItemListInTrack", timeline.GetItemListInTrack,
                             "video", 1) or []
        if items:
            return "\n".join(said + ["La timeline de mesure est déjà prête."])
    if media is None:
        raise ProbeError("Le projet de mesure n'a pas de plan : donne un "
                         "média avec --media.")
    if not media.is_file():
        raise ProbeError(f"Média introuvable : {media}.")
    pool = session.call("GetMediaPool", project.GetMediaPool)
    # The README of 21.1 deprecates ImportMedia([paths]) for
    # ImportMedia([{"FilePath": path}]); on 21.1.0.17 the latter imported
    # nothing and returned an empty list (10 October 2026), the former works.
    clips = session.call("ImportMedia([{FilePath}])", pool.ImportMedia,
                         [{"FilePath": str(media)}]) or []
    if not clips:
        clips = session.call("ImportMedia([path])", pool.ImportMedia,
                             [str(media)]) or []
    if not clips:
        raise ProbeError(f"Resolve n'a pas importé {media}.")
    created = session.call("CreateTimelineFromClips",
                           pool.CreateTimelineFromClips, timeline_name,
                           [{"mediaPoolItem": clips[0]}])
    if created is None:
        raise ProbeError("Resolve n'a pas créé la timeline de mesure.")
    return "\n".join(said + [f"Timeline « {timeline_name} » créée avec "
                              f"{media.name}."])


def run_check(session: Session, project_name: str) -> dict[str, Any]:
    project = measurement_project(session, project_name)
    facts = project_facts(session, project)
    timeline = session.call("GetCurrentTimeline", project.GetCurrentTimeline)
    facts["timeline"] = None
    if timeline is not None:
        items = session.call("GetItemListInTrack", timeline.GetItemListInTrack,
                             "video", 1) or []
        facts["timeline"] = {
            "name": session.call("Timeline.GetName", timeline.GetName),
            "v1_items": len(items),
        }
        if items:
            facts["get_cdl"] = read_back_cdl(session, items[0])
    return facts


# --- Reading the records back ------------------------------------------------

def load_records(out_dir: Path) -> list[dict[str, Any]]:
    path = Path(out_dir) / RECORDS
    if not path.is_file():
        raise ProbeError(f"Aucune mesure dans {out_dir} ({RECORDS} absent).")
    with open(path, encoding="utf-8") as file:
        return [json.loads(line) for line in file if line.strip()]


def build_report(out_dir: Path, tolerance: float = DEFAULT_TOLERANCE
                 ) -> dict[str, Any]:
    out_dir = Path(out_dir)
    records = load_records(out_dir)
    weights = None
    for record in records:
        if (record["kind"] == "cdl" and record.get("exported") and
                Cdl.from_json(record["cdl"]) == Cdl(saturation=0.0)):
            weights = measured_luma_weights(read_cube(out_dir /
                                                      record["lut"]))
    variants = VARIANTS + (measured_variants(weights) if weights else ())
    session = None
    entries = []
    for record in records:
        if record["kind"] == "session":
            session = record
            continue
        entry = {"kind": record["kind"],
                 "session": None if session is None else {
                     "resolve": session.get("resolve"),
                     "lut_size": session.get("lut_size"),
                     "settings": session.get("settings")}}
        if record["kind"] == "cdl":
            entry.update(case=record["case"], question=record["question"],
                         cdl=record["cdl"], get_cdl=record.get("get_cdl"))
        else:
            entry.update(label=record["label"], note=record.get("note", ""))
        if not record.get("exported"):
            entry["error"] = "LUT non exporté"
            entries.append(entry)
            continue
        cube = read_cube(out_dir / record["lut"])
        if record["kind"] == "cdl":
            ranked = compare_cdl(cube, Cdl.from_json(record["cdl"]), variants)
            by_name = {variant.name: found for variant, found in ranked}
            entry["closest"] = [
                {"variant": variant.name, **found.as_json()}
                for variant, found in ranked[:3]]
            entry["matches"] = [variant.name for variant, found in ranked
                                if found.max_abs <= tolerance]
            entry["engine"] = by_name[ENGINE_IN_CUBE.name].as_json()
            entry["engine_unclamped"] = by_name[ENGINE_VARIANT.name].as_json()
        else:
            entry["description"] = describe(cube)
        entries.append(entry)
    cdl_entries = [e for e in entries if e["kind"] == "cdl" and "matches" in e]
    common = None
    for entry in cdl_entries:
        names = set(entry["matches"])
        common = names if common is None else common & names
    return {"tolerance": tolerance, "entries": entries,
            "measured_luma_weights": list(weights) if weights else None,
            "formulas_matching_every_case": sorted(common or []),
            "engine_matches_every_case": bool(cdl_entries) and all(
                entry["engine"]["max_abs"] <= tolerance
                for entry in cdl_entries)}


def summary_lines(report: dict[str, Any]) -> list[str]:
    lines = [f"Tolérance : {report['tolerance']:g} "
             f"({report['tolerance'] * 1023:.2f} code 10 bits)."]
    for entry in report["entries"]:
        title = entry.get("case") or f"main : {entry.get('label')}"
        if "error" in entry:
            lines.append(f"- {title} : {entry['error']}")
        elif entry["kind"] == "cdl":
            best = entry["closest"][0]
            engine = entry["engine"]["max_10bit_codes"]
            matches = entry["matches"]
            listed = ", ".join(matches[:4]) + (", …" if len(matches) > 4
                                               else "")
            lines.append(
                f"- {title} : plus proche {best['variant']} "
                f"(écart max {best['max_abs']:.2e}) ; moteur {engine:.2f} "
                f"code ; dans la tolérance : "
                f"{len(matches)} ({listed or 'aucune'})")
        else:
            identity = entry["description"]["identity"]
            spread = entry["description"]["channel_spread"]
            lines.append(
                f"- {title} : écart à l'identité {identity['max_abs']:.3f}, "
                f"dépendance aux autres canaux "
                f"{max(spread):.2e}")
    weights = report.get("measured_luma_weights")
    if weights:
        lines.append("Poids de la luma mesurés (saturation 0) : R {:.4f}, "
                     "G {:.4f}, B {:.4f} (formules « mesure »).".format(
                         *weights))
    common = report["formulas_matching_every_case"]
    lines.append("Formules qui tiennent sur tous les cas : "
                 f"{', '.join(common) or 'aucune'}.")
    lines.append("La formule du moteur (ColorCdl.h), écrêtée à [0, 1] comme "
                 "le cube, tient sur tous les cas : "
                 f"{'oui' if report['engine_matches_every_case'] else 'non'}.")
    return lines


# --- Command line ------------------------------------------------------------

def connect() -> Any:  # pragma: no cover - needs Resolve Studio running
    sidecar = Path(__file__).resolve().parents[2] / "sidecar"
    if str(sidecar) not in sys.path:
        sys.path.insert(0, str(sidecar))
    from resolve_bridge import ResolveBridgeError, connect as bridge_connect
    try:
        return bridge_connect()
    except ResolveBridgeError as error:
        raise ProbeError(str(error)) from error


def output_dir(text: str) -> Path:
    path = Path(text).expanduser().resolve()
    path.mkdir(parents=True, exist_ok=True)
    return path


def main(argv: list[str] | None = None) -> int:  # pragma: no cover
    parser = argparse.ArgumentParser(
        description="Sonde GR12 : ce que DaVinci Resolve fait d'une CDL, "
                    "mesuré par ses LUT exportés.")
    commands = parser.add_subparsers(dest="command", required=True)

    def resolve_command(name: str, help_text: str) -> argparse.ArgumentParser:
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--project", default=DEFAULT_PROJECT)
        command.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
        return command

    resolve_command("check", "l'état de Resolve et du projet de mesure")
    setup = resolve_command("setup", "prépare le projet de mesure")
    setup.add_argument("--media", help="le plan à mesurer, s'il manque")
    setup.add_argument("--create-project", action="store_true",
                       help="ouvre ou crée le projet de mesure ; seulement "
                            "sans travail non enregistré ouvert dans Resolve")
    setup.add_argument("--color-science", choices=COLOR_SCIENCES)
    cdl = resolve_command("cdl", "pose chaque CDL et exporte son LUT")
    cdl.add_argument("--out", required=True)
    cdl.add_argument("--lut-size", type=int, default=33)
    cdl.add_argument("--from-export",
                     help="réponse JSON d'export_cdl : ses plans à la place "
                          "des cas de la sonde")
    cdl.add_argument("--only", nargs="*", help="noms de cas")
    manual = resolve_command("export",
                             "exporte le LUT du plan tel qu'il est réglé")
    manual.add_argument("--out", required=True)
    manual.add_argument("--label", required=True)
    manual.add_argument("--note", default="")
    manual.add_argument("--lut-size", type=int, default=33)
    compare = commands.add_parser("compare", help="compare les LUT mesurés")
    compare.add_argument("out")
    compare.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE)
    args = parser.parse_args(argv)

    try:
        if args.command == "compare":
            report = build_report(Path(args.out), args.tolerance)
            with open(Path(args.out) / REPORT, "w", encoding="utf-8") as file:
                json.dump(report, file, ensure_ascii=False, indent=2)
            print("\n".join(summary_lines(report)))
            return 0
        out_dir = output_dir(args.out) if hasattr(args, "out") else None
        session = Session(connect(), out_dir, args.timeout)
        if args.command == "check":
            print(json.dumps(run_check(session, args.project),
                             ensure_ascii=False, indent=2))
        elif args.command == "setup":
            media = (Path(args.media).expanduser().resolve()
                     if args.media else None)
            print(run_setup(session, args.project, media,
                            args.create_project, args.color_science))
        elif args.command == "cdl":
            if args.from_export:
                with open(args.from_export, encoding="utf-8") as file:
                    cases = cases_from_export(json.load(file))
            else:
                cases = list(CDL_CASES)
            if args.only:
                unknown = set(args.only) - {case.name for case in cases}
                if unknown:
                    raise ProbeError("Cas inconnus : " +
                                     ", ".join(sorted(unknown)) + ".")
                cases = [case for case in cases if case.name in args.only]
            records = run_cdl_sweep(session, args.project, cases,
                                    args.lut_size)
            failed = [r["case"] for r in records if not r.get("exported")]
            print(f"Terminé : {len(records) - len(failed)} LUT exportés "
                  f"dans {out_dir}." +
                  (f" Échecs : {', '.join(failed)}." if failed else ""))
        elif args.command == "export":
            entry = run_manual_export(session, args.project, args.label,
                                      args.note, args.lut_size)
            print(f"Terminé : {entry['lut']}" if entry["exported"]
                  else "Resolve n'a pas exporté le LUT.")
        return 0
    except ProbeError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
