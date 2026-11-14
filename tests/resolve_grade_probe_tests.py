#!/usr/bin/env python3
"""GR12 -- the Resolve grade probe without Resolve: the cube reader, the
candidate CDL formulas, the comparison, and the sweep against fakes."""

import importlib.util
import itertools
import sys
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "resolve-grade-probe" / "grade_probe.py"
SPEC = importlib.util.spec_from_file_location("grade_probe", MODULE_PATH)
PROBE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
# Dataclasses look their module up while being built.
sys.modules[SPEC.name] = PROBE
SPEC.loader.exec_module(PROBE)

# A 9-point lattice is a subset of Resolve's 17-, 33- and 65-point ones, so a
# difference seen on it is seen on theirs; it keeps the tests fast.
SMALL = 9


def variant(name):
    base, weights, output = name.split("/")
    return PROBE.Variant(base, weights, output)


class FakeItem:
    """A timeline item whose exported LUT is `formula` applied to the last
    CDL set, the way Resolve's would be under the formula it really uses."""

    def __init__(self, formula, get_cdl=None):
        self.formula = formula
        self.cdl = PROBE.NEUTRAL
        self.calls = []
        self.resolve = None
        if get_cdl is not None:
            self.GetCDL = get_cdl

    def SetCDL(self, mapping):
        self.calls.append(("SetCDL", dict(mapping)))
        self.cdl = PROBE.Cdl.from_resolve_map(mapping)
        return True

    def ExportLUT(self, export_type, path):
        self.calls.append(("ExportLUT", export_type, path))
        # Like Resolve 21.1: nothing outside the Color page.
        if self.resolve is not None and self.resolve.page != "color":
            return False
        cdl = self.cdl
        Path(path).write_text(PROBE.cube_text(
            SMALL, lambda rgb: PROBE.apply_cdl(cdl, rgb, self.formula)))
        return True


class FakeTimeline:
    def __init__(self, items):
        self.items = items

    def GetItemListInTrack(self, kind, index):
        return list(self.items) if (kind, index) == ("video", 1) else []

    def GetName(self):
        return "Sonde étalonnage"


class FakePool:
    """Imports like Resolve 21.1.0.17: a list of paths, not the dicts its
    README recommends."""

    def __init__(self, project):
        self.project = project
        self.calls = []

    def ImportMedia(self, infos):
        self.calls.append(("ImportMedia", infos))
        return [] if isinstance(infos[0], dict) else ["clip"]

    def CreateTimelineFromClips(self, name, infos):
        self.calls.append(("CreateTimelineFromClips", name, infos))
        self.project.timeline = FakeTimeline([FakeItem(PROBE.ENGINE_VARIANT)])
        return self.project.timeline


class FakeProject:
    def __init__(self, name, timeline, takes=None):
        self.name = name
        self.timeline = timeline
        self.pool = FakePool(self)
        self.settings = {"colorScienceMode": "davinciYRGB"}
        # The colour sciences this project accepts; None for all of them.
        self.takes = takes

    def GetName(self):
        return self.name

    def GetCurrentTimeline(self):
        return self.timeline

    def GetSettings(self):
        return dict(self.settings)

    def SetSettings(self, settings):
        mode = settings.get("colorScienceMode")
        if self.takes is not None and mode not in self.takes:
            return False
        self.settings.update(settings)
        return True

    def GetMediaPool(self):
        return self.pool


class FakeManager:
    def __init__(self, project, others=()):
        self.project = project
        self.projects = {p.name: p for p in (project, *others) if p}
        self.calls = []

    def GetCurrentProject(self):
        return self.project

    def GetProjectListInCurrentFolder(self):
        return list(self.projects)

    def LoadProject(self, name):
        self.calls.append(("LoadProject", name))
        self.project = self.projects.get(name)
        return self.project

    def CreateProject(self, name):
        self.calls.append(("CreateProject", name))
        if name in self.projects:
            return None
        self.project = self.projects[name] = FakeProject(name, None)
        return self.project


class FakeResolve:
    EXPORT_LUT_17PTCUBE = 17.0
    EXPORT_LUT_33PTCUBE = 33.0
    EXPORT_LUT_65PTCUBE = 65.0

    def __init__(self, project, others=()):
        self.manager = FakeManager(project, others)
        self.page = "edit"
        self.opened = []

    def GetCurrentPage(self):
        return self.page

    def OpenPage(self, page):
        self.opened.append(page)
        self.page = page
        return True

    def GetProjectManager(self):
        return self.manager

    def GetVersionString(self):
        return "21.1.0.17"

    def GetProductName(self):
        return "DaVinci Resolve Studio"


def no_watchdog(code):
    raise AssertionError(f"the watchdog fired ({code})")


def session_for(resolve, out_dir):
    return PROBE.Session(resolve, out_dir, timeout=10, leave=no_watchdog,
                         echo=lambda text: None)


def setup_with(formula, name=PROBE.DEFAULT_PROJECT, get_cdl=None):
    item = FakeItem(formula, get_cdl)
    project = FakeProject(name, FakeTimeline([item]))
    resolve = FakeResolve(project)
    item.resolve = resolve
    return resolve, project, item


class CubeTests(unittest.TestCase):
    def test_reads_red_fastest_and_the_domain(self):
        lines = ["TITLE \"t\"", "# a comment", "LUT_3D_SIZE 2",
                 "DOMAIN_MIN 0 0 0", "DOMAIN_MAX 2 2 2"]
        lines += [f"{i} {i} {i}  # entry" for i in range(8)]
        cube = PROBE.parse_cube("\n".join(lines))
        self.assertEqual(cube.size, 2)
        self.assertEqual(cube.output_at(1, 0, 0), (1.0, 1.0, 1.0))
        self.assertEqual(cube.output_at(0, 1, 0), (2.0, 2.0, 2.0))
        self.assertEqual(cube.output_at(0, 0, 1), (4.0, 4.0, 4.0))
        self.assertEqual(cube.input_at(1, 1, 1), (2.0, 2.0, 2.0))

    def test_input_range_keyword(self):
        cube = PROBE.parse_cube("LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 0 4\n" +
                                "0 0 0\n" * 8)
        self.assertEqual(cube.input_at(1, 0, 1), (4.0, 0.0, 4.0))

    def test_refuses_what_it_cannot_read(self):
        with self.assertRaises(PROBE.ProbeError):
            PROBE.parse_cube("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n")
        with self.assertRaises(PROBE.ProbeError):
            PROBE.parse_cube("LUT_3D_SIZE 2\n" + "0 0 0\n" * 7)
        with self.assertRaises(PROBE.ProbeError):
            PROBE.parse_cube("LUT_3D_SIZE 2\nSHAPER 1\n" + "0 0 0\n" * 8)

    def test_writes_what_it_reads(self):
        cube = PROBE.parse_cube(PROBE.cube_text(3, lambda rgb: rgb))
        self.assertEqual(PROBE.deviation(cube, lambda rgb: rgb).max_abs, 0.0)


class CdlTests(unittest.TestCase):
    def test_resolve_map_is_the_engines(self):
        cdl = PROBE.Cdl((1.25, 1.0, 0.8), (0.0, -0.1, 0.05), (1.0, 1.4, 0.7),
                        1.5)
        mapping = cdl.resolve_map()
        self.assertEqual(mapping, {
            "NodeIndex": "1", "Slope": "1.25 1 0.8",
            "Offset": "0 -0.1 0.05", "Power": "1 1.4 0.7",
            "Saturation": "1.5"})
        self.assertEqual(PROBE.Cdl.from_resolve_map(mapping), cdl)

    def test_below_zero_before_the_power(self):
        cdl = PROBE.Cdl(offset=(-0.1,) * 3, power=(1.5,) * 3)
        black = (0.0, 0.0, 0.0)
        self.assertAlmostEqual(
            PROBE.apply_cdl(cdl, black, variant("pass/rec709/none"))[0], -0.1)
        self.assertEqual(
            PROBE.apply_cdl(cdl, black, variant("zero/rec709/none"))[0], 0.0)
        self.assertEqual(
            PROBE.apply_cdl(cdl, black, variant("unit/rec709/none"))[0], 0.0)

    def test_saturation_zero_is_the_luma(self):
        cdl = PROBE.Cdl(saturation=0.0)
        for name, weights in PROBE.LUMA_WEIGHTS.items():
            grey = PROBE.apply_cdl(cdl, (1.0, 0.0, 0.0),
                                   variant(f"pass/{name}/none"))
            for value in grey:
                self.assertAlmostEqual(value, weights[0])

    def test_the_cases_tell_every_formula_apart(self):
        lattice = [(r / (SMALL - 1), g / (SMALL - 1), b / (SMALL - 1))
                   for r in range(SMALL) for g in range(SMALL)
                   for b in range(SMALL)]
        outputs = {
            formula.name: [[PROBE.apply_cdl(case.cdl, rgb, formula)
                            for rgb in lattice] for case in PROBE.CDL_CASES]
            for formula in PROBE.VARIANTS}
        margin = 10 * PROBE.DEFAULT_TOLERANCE
        for first, second in itertools.combinations(sorted(outputs), 2):
            apart = max(
                abs(a - b)
                for case_a, case_b in zip(outputs[first], outputs[second])
                for rgb_a, rgb_b in zip(case_a, case_b)
                for a, b in zip(rgb_a, rgb_b))
            self.assertGreater(apart, margin, f"{first} vs {second}")

    def test_the_neutral_case_is_the_identity_for_every_formula(self):
        for formula in PROBE.VARIANTS:
            for rgb in [(0.0, 0.5, 1.0), (0.25, 0.75, 0.125)]:
                for got, wanted in zip(
                        PROBE.apply_cdl(PROBE.NEUTRAL, rgb, formula), rgb):
                    self.assertAlmostEqual(got, wanted)

    def test_cases_from_an_export_cdl_reply(self):
        reply = {"space": "acescct", "clips": [{
            "clip_id": "01J", "media": "A001.mov",
            "cdl": {"slope": [1.1, 1, 0.9], "offset": [0.01, 0, 0],
                    "power": [1, 1, 1.2], "saturation": 1.05},
            "resolve_set_cdl": {"NodeIndex": "1", "Slope": "1.1 1 0.9",
                                "Offset": "0.01 0 0", "Power": "1 1 1.2",
                                "Saturation": "1.05"}}]}
        cases = PROBE.cases_from_export(reply)
        self.assertEqual([case.name for case in cases], ["plan_01"])
        self.assertEqual(cases[0].cdl.slope, (1.1, 1.0, 0.9))
        self.assertIn("A001.mov", cases[0].question)
        reply["clips"][0]["resolve_set_cdl"]["Power"] = "1 1 1.3"
        with self.assertRaises(PROBE.ProbeError):
            PROBE.cases_from_export(reply)
        with self.assertRaises(PROBE.ProbeError):
            PROBE.cases_from_export({"clips": []})


class ComparisonTests(unittest.TestCase):
    def test_finds_the_formula_that_made_the_cube(self):
        case = next(c for c in PROBE.CDL_CASES if c.name == "combinee")
        for name in ("pass/rec709/none", "unit/rec601/unit",
                     "zero/rec709/unit"):
            formula = variant(name)
            cube = PROBE.parse_cube(PROBE.cube_text(
                SMALL, lambda rgb: PROBE.apply_cdl(case.cdl, rgb, formula)))
            ranked = PROBE.compare_cdl(cube, case.cdl)
            self.assertLessEqual(ranked[0][1].max_abs, 5.1e-7)
            closest = [v.name for v, d in ranked if d.max_abs <= 5.1e-7]
            self.assertIn(name, closest)

    def test_describe_sees_channel_mixing(self):
        def power(rgb):
            return tuple(value ** 1.2 for value in rgb)
        separable = PROBE.describe(PROBE.parse_cube(
            PROBE.cube_text(SMALL, power)))
        self.assertLess(max(separable["channel_spread"]), 1e-6)
        self.assertGreater(separable["identity"]["max_abs"], 0.01)
        self.assertEqual(separable["grey_axis"][-1]["input"], 1.0)
        mixed = PROBE.describe(PROBE.parse_cube(PROBE.cube_text(
            SMALL, lambda rgb: PROBE.apply_cdl(
                PROBE.Cdl(saturation=0.5), rgb, PROBE.ENGINE_VARIANT))))
        self.assertGreater(max(mixed["channel_spread"]), 0.1)


class WatchdogTests(unittest.TestCase):
    def test_fires_when_the_call_hangs_and_not_otherwise(self):
        fired = []
        with PROBE.guarded("SetCDL", 0.05, fired.append):
            time.sleep(0.3)
        self.assertEqual(fired, [124])
        fired.clear()
        with PROBE.guarded("SetCDL", 0.05, fired.append):
            pass
        time.sleep(0.2)
        self.assertEqual(fired, [])


class SweepTests(unittest.TestCase):
    def test_refuses_any_other_project(self):
        resolve, _, item = setup_with(PROBE.ENGINE_VARIANT, name="Mon film")
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(PROBE.ProbeError) as raised:
                PROBE.run_cdl_sweep(session_for(resolve, Path(directory)),
                                    PROBE.DEFAULT_PROJECT,
                                    list(PROBE.CDL_CASES), 17)
        self.assertIn("Mon film", str(raised.exception))
        self.assertEqual(item.calls, [])

    def test_measures_every_case_then_leaves_the_clip_neutral(self):
        formula = variant("zero/rec709/unit")
        resolve, _, item = setup_with(formula)
        with tempfile.TemporaryDirectory() as directory:
            out_dir = Path(directory)
            records = PROBE.run_cdl_sweep(session_for(resolve, out_dir),
                                          PROBE.DEFAULT_PROJECT,
                                          list(PROBE.CDL_CASES), 17)
            self.assertEqual(len(records), len(PROBE.CDL_CASES))
            self.assertTrue(all(r["exported"] for r in records))
            self.assertEqual(resolve.opened, ["color"])
            self.assertEqual(item.calls[-1],
                             ("SetCDL", PROBE.NEUTRAL.resolve_map()))
            exports = [c for c in item.calls if c[0] == "ExportLUT"]
            self.assertTrue(all(c[1] == 17.0 for c in exports))
            stored = PROBE.load_records(out_dir)
            self.assertEqual(stored[0]["kind"], "session")
            self.assertEqual(stored[0]["settings"]["colorScienceMode"],
                             "davinciYRGB")
            self.assertEqual(stored[0]["product"], "DaVinci Resolve Studio")
            journal = (out_dir / PROBE.JOURNAL).read_text(encoding="utf-8")
            self.assertIn("-> ExportLUT (forte)", journal)
            report = PROBE.build_report(out_dir)
        # The weights measured on the fake are Rec.709's, so their variant
        # holds as well.
        self.assertEqual(report["formulas_matching_every_case"],
                         ["zero/mesure/unit", "zero/rec709/unit"])
        self.assertFalse(report["engine_matches_every_case"])
        self.assertEqual(records[0]["get_cdl"], {"available": False})
        summary = "\n".join(PROBE.summary_lines(report))
        self.assertIn("zero/rec709/unit", summary)

    def test_luma_weights_are_measured_on_the_saturation_0_case(self):
        weights = (0.21, 0.70, 0.09)
        formula = PROBE.Variant("pass", "x", "unit", weights)
        resolve, _, _ = setup_with(formula)
        with tempfile.TemporaryDirectory() as directory:
            out_dir = Path(directory)
            PROBE.run_cdl_sweep(session_for(resolve, out_dir),
                                PROBE.DEFAULT_PROJECT,
                                list(PROBE.CDL_CASES), 17)
            report = PROBE.build_report(out_dir)
        for got, wanted in zip(report["measured_luma_weights"], weights):
            self.assertAlmostEqual(got, wanted, places=5)
        self.assertEqual(report["formulas_matching_every_case"],
                         ["pass/mesure/unit"])
        summary = "\n".join(PROBE.summary_lines(report))
        self.assertIn("R 0.2100, G 0.7000, B 0.0900", summary)

    def test_no_weights_when_the_primaries_are_not_grey(self):
        cube = PROBE.parse_cube(PROBE.cube_text(3, lambda rgb: rgb))
        self.assertIsNone(PROBE.measured_luma_weights(cube))

    def test_the_engines_formula_is_recognised(self):
        resolve, _, _ = setup_with(PROBE.ENGINE_IN_CUBE)
        with tempfile.TemporaryDirectory() as directory:
            out_dir = Path(directory)
            PROBE.run_cdl_sweep(session_for(resolve, out_dir),
                                PROBE.DEFAULT_PROJECT,
                                list(PROBE.CDL_CASES[:4]), 33)
            report = PROBE.build_report(out_dir)
        self.assertTrue(report["engine_matches_every_case"])
        self.assertIn(PROBE.ENGINE_IN_CUBE.name,
                      report["formulas_matching_every_case"])

    def test_records_what_get_cdl_gives(self):
        resolve, _, _ = setup_with(
            PROBE.ENGINE_VARIANT,
            get_cdl=lambda node: {"Slope": 1, "Offset": "0 0 0"})
        with tempfile.TemporaryDirectory() as directory:
            records = PROBE.run_cdl_sweep(
                session_for(resolve, Path(directory)), PROBE.DEFAULT_PROJECT,
                list(PROBE.CDL_CASES[:1]), 17)
        self.assertEqual(records[0]["get_cdl"], {
            "available": True, "args": [1],
            "value": {"Slope": "1", "Offset": "0 0 0"}})

    def test_get_cdl_without_a_node_number(self):
        def get_cdl(*args):
            if args:
                raise TypeError("GetCDL() takes no arguments")
            return {"Slope": "1 1 1"}
        resolve, _, item = setup_with(PROBE.ENGINE_VARIANT, get_cdl=get_cdl)
        found = PROBE.read_back_cdl(session_for(resolve, None), item)
        self.assertEqual(found["args"], [])
        resolve, _, item = setup_with(PROBE.ENGINE_VARIANT,
                                      get_cdl=lambda *args: None)
        found = PROBE.read_back_cdl(session_for(resolve, None), item)
        self.assertEqual(found, {"available": False,
                                 "attempts": [None, None]})

    def test_manual_export_is_described(self):
        resolve, _, item = setup_with(PROBE.ENGINE_VARIANT)
        item.cdl = PROBE.Cdl(power=(1.3, 1.3, 1.3))
        with tempfile.TemporaryDirectory() as directory:
            out_dir = Path(directory)
            entry = PROBE.run_manual_export(
                session_for(resolve, out_dir), PROBE.DEFAULT_PROJECT,
                "gamma maître -0,10", "roue Gamma", 17)
            self.assertEqual(entry["lut"], "main_gamma_maître_-0_10.cube")
            self.assertTrue((out_dir / entry["lut"]).is_file())
            report = PROBE.build_report(out_dir)
        description = report["entries"][0]["description"]
        self.assertGreater(description["identity"]["max_abs"], 0.01)
        self.assertLess(max(description["channel_spread"]), 1e-6)
        self.assertEqual(report["entries"][0]["label"], "gamma maître -0,10")

    def test_setup_builds_the_timeline_only_when_missing(self):
        project = FakeProject(PROBE.DEFAULT_PROJECT, None)
        resolve = FakeResolve(project)
        with tempfile.NamedTemporaryFile(suffix=".mov") as media:
            message = PROBE.run_setup(session_for(resolve, None),
                                      PROBE.DEFAULT_PROJECT,
                                      Path(media.name))
            self.assertIn("créée", message)
            self.assertEqual(project.pool.calls[:2], [
                ("ImportMedia", [{"FilePath": media.name}]),
                ("ImportMedia", [media.name])])
            self.assertEqual(project.pool.calls[2][2],
                             [{"mediaPoolItem": "clip"}])
            again = PROBE.run_setup(session_for(resolve, None),
                                    PROBE.DEFAULT_PROJECT, Path(media.name))
        self.assertIn("déjà prête", again)
        self.assertEqual(len(project.pool.calls), 3)

    def test_setup_without_media_when_the_clip_is_missing(self):
        resolve = FakeResolve(FakeProject(PROBE.DEFAULT_PROJECT, None))
        with self.assertRaises(PROBE.ProbeError):
            PROBE.run_setup(session_for(resolve, None), PROBE.DEFAULT_PROJECT,
                            None)

    def test_creates_the_project_only_when_told(self):
        resolve = FakeResolve(FakeProject("Untitled Project", None))
        with self.assertRaises(PROBE.ProbeError):
            PROBE.run_setup(session_for(resolve, None), PROBE.DEFAULT_PROJECT,
                            None)
        self.assertEqual(resolve.manager.calls, [])
        with tempfile.NamedTemporaryFile(suffix=".mov") as media:
            said = PROBE.run_setup(session_for(resolve, None),
                                   PROBE.DEFAULT_PROJECT, Path(media.name),
                                   create_project=True,
                                   color_science="acescct")
        self.assertEqual(resolve.manager.calls,
                         [("CreateProject", PROBE.DEFAULT_PROJECT)])
        project = resolve.manager.project
        self.assertEqual(project.settings["colorScienceMode"], "acescct")
        self.assertIn("créé", said)
        self.assertIn("acescct", said)

    def test_loads_the_project_when_it_exists(self):
        measured = FakeProject(PROBE.DEFAULT_PROJECT,
                               FakeTimeline([FakeItem(PROBE.ENGINE_VARIANT)]))
        resolve = FakeResolve(FakeProject("Mon film", None), [measured])
        said = PROBE.run_setup(session_for(resolve, None),
                               PROBE.DEFAULT_PROJECT, None,
                               create_project=True)
        self.assertEqual(resolve.manager.calls,
                         [("LoadProject", PROBE.DEFAULT_PROJECT)])
        self.assertIn("déjà prête", said)
        again = PROBE.run_setup(session_for(resolve, None),
                                PROBE.DEFAULT_PROJECT, None,
                                create_project=True)
        self.assertIn("déjà ouvert", again)
        self.assertEqual(len(resolve.manager.calls), 1)

    def test_a_colour_science_not_taken_is_said(self):
        project = FakeProject(PROBE.DEFAULT_PROJECT,
                              FakeTimeline([FakeItem(PROBE.ENGINE_VARIANT)]),
                              takes=("davinciYRGB",))
        resolve = FakeResolve(project)
        with self.assertRaises(PROBE.ProbeError) as raised:
            PROBE.run_setup(session_for(resolve, None), PROBE.DEFAULT_PROJECT,
                            None, color_science="acescct")
        self.assertIn("davinciYRGB", str(raised.exception))

    def test_lut_sizes_are_resolves(self):
        resolve, _, _ = setup_with(PROBE.ENGINE_VARIANT)
        self.assertEqual(PROBE.lut_export_type(resolve, 65), 65.0)
        with self.assertRaises(PROBE.ProbeError):
            PROBE.lut_export_type(resolve, 32)


if __name__ == "__main__":
    unittest.main()
