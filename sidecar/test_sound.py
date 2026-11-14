"""SN3 -- the sound renderer seen from the sidecar, against the projects the
engine wrote for tools/cutmachine-sound/test/fixtures/."""

from __future__ import annotations

import hashlib
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from sidecar.binary import CutmachineBinary
from sidecar.repl import run_sound
from sidecar.sound import SoundError, SoundRenderer, place_stem

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tools" / "cutmachine-sound" / "test" / "fixtures" / "projet-25"
NODE = shutil.which("node")
BINARY = ROOT / "build" / "cutmachine"

DITTY = """
input.niveau = 0.5;
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
loop(() => { for (const c of edit.cuts) at(c, () => clic.play(0, { attack: 0, release: 0.001, amp: input.niveau })); }, { name: 'clics' });
"""


@unittest.skipUnless(NODE, "Node is not installed")
class SoundRendererTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        base = Path(self.directory.name)
        self.package = base / "Film.cutmachine-project"
        self.package.mkdir()
        shutil.copy(FIXTURE / "project.cutmachine.json", self.package)
        self.ditty = base / "clics.js"
        self.ditty.write_text(DITTY, encoding="utf-8")
        self.sound = SoundRenderer()

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_stem_lands_in_the_package_with_its_report(self) -> None:
        result = self.sound.render(self.ditty, self.package, seed=3, settings={"niveau": 0.25})
        self.assertEqual(result.stem, self.package / "Sound" / "clics-seed3.wav")
        self.assertTrue(result.stem.is_file())
        report = result.report
        self.assertEqual(report["seed"], 3)
        self.assertEqual(report["inputs"], {"niveau": 0.25})
        self.assertEqual(report["edit"]["cuts"], 5)
        self.assertEqual(report["edit"]["start"], "01:00:00:00")
        self.assertEqual(report["timeReference"], 90000 * 1920)
        self.assertEqual(report["peakDb"], -12.0)
        data = result.stem.read_bytes()
        self.assertEqual(hashlib.sha256(data[data.index(b"data") + 8:]).hexdigest(), report["sha256"])

    def test_same_inputs_same_bytes(self) -> None:
        first = self.sound.render(self.ditty, self.package, out=Path(self.directory.name) / "a.wav")
        second = self.sound.render(self.ditty, self.package / "project.cutmachine.json",
                                   out=Path(self.directory.name) / "b.wav")
        self.assertEqual(first.stem.read_bytes(), second.stem.read_bytes())

    def test_the_edit_carries_ids_and_frames(self) -> None:
        edit = self.sound.edit(self.package)
        clips = json.loads((self.package / "project.cutmachine.json").read_text())["timelines"][0]["tracks"][0]["clips"]
        self.assertEqual(edit["cuts"][0]["id"], f"{clips[0]['id']}>{clips[1]['id']}")
        self.assertEqual([c["frame"] for c in edit["cuts"]], [46, 63, 75, 136, 166])

    def test_loop_stems_are_listed(self) -> None:
        result = self.sound.render(self.ditty, self.package,
                                   loop_stems=Path(self.directory.name) / "boucles")
        self.assertEqual([p.name for p in result.loop_stems], ["01-clics.wav"])
        self.assertTrue(result.loop_stems[0].is_file())

    def test_refusals_are_named(self) -> None:
        with self.assertRaisesRegex(SoundError, 'no timeline "NOPE"'):
            self.sound.render(self.ditty, self.package, timeline="NOPE")
        with self.assertRaisesRegex(SoundError, "not a ditty input name"):
            self.sound.render(self.ditty, self.package, settings={"1x": 2})
        with self.assertRaisesRegex(SoundError, "a number or a string, not bool"):
            self.sound.render(self.ditty, self.package, settings={"niveau": True})
        with self.assertRaisesRegex(SoundError, "not a finite number"):
            self.sound.render(self.ditty, self.package, settings={"niveau": float("nan")})
        with self.assertRaisesRegex(SoundError, "say where the stem goes"):
            self.sound.render(self.ditty, FIXTURE.parent / "projet-25" / "film_V1.edl")
        with self.assertRaisesRegex(SoundError, "no ditty at"):
            self.sound.render(self.package / "absent.js", self.package)
        broken = Path(self.directory.name) / "casse.js"
        broken.write_text("loop(() => { throw new Error('boum'); });", encoding="utf-8")
        with self.assertRaisesRegex(SoundError, 'loop "loop 0" failed: boum'):
            self.sound.render(broken, self.package)


    def test_a_partition_plays_with_an_instrument_set(self) -> None:
        sound = ROOT / "tools" / "cutmachine-sound"
        partition = Path(self.directory.name) / "impacts-projet.json"
        edit = self.sound.edit(self.package)
        partition.write_text(json.dumps({
            "format": "cutmachine-partition", "version": 1,
            "tracks": [{"name": "impacts", "instrument": "impact",
                        "events": [{"at": {"cut": c["id"]}, "params": {"poids": 0.5}}
                                   for c in edit["cuts"] if c["type"] == "cut"]}],
        }), encoding="utf-8")
        result = self.sound.render(sound / "instruments" / "verre.js", self.package,
                                   partition=partition)
        self.assertEqual(result.stem.name, "impacts-projet-verre-seed1.wav")
        loops = result.report["loops"]
        self.assertEqual(loops[0]["instrument"], "impact")
        self.assertEqual(loops[0]["triggers"], [c["frame"] * 1920 for c in edit["cuts"]
                                                if c["type"] == "cut"])
        with self.assertRaisesRegex(SoundError, "no partition at"):
            self.sound.render(self.ditty, self.package, partition=self.package / "x.json")

    def test_repl_command_renders_and_reports(self) -> None:
        printed: list[str] = []
        run_sound(self.sound, self.package, f" {self.ditty} graine=7 niveau=0.5", printed.append)
        self.assertEqual(printed[0], f"Stem : {self.package / 'Sound' / 'clics-seed7.wav'}")
        summary = json.loads(printed[1])
        self.assertEqual(summary["seed"], 7)
        self.assertEqual(summary["inputs"], {"niveau": 0.5})
        printed.clear()
        run_sound(self.sound, self.package, f" {self.ditty} niveau", printed.append)
        self.assertEqual(printed, ["Réglage illisible : 'niveau' (nom=valeur attendu)"])
        printed.clear()
        run_sound(self.sound, self.package, f" {self.ditty} --timeline=x", printed.append)
        self.assertTrue(printed[0].startswith("Rendu refusé : "), printed)


def write_stem(path: Path, *, rate: int, samples: int, reference: int) -> None:
    """A 24-bit stereo WAV laid out as tools/cutmachine-sound/wav.mjs writes
    it: bext (TimeReference at 338), fmt, data of silence."""
    bext = bytearray(602)
    bext[338:346] = reference.to_bytes(8, "little")
    data = bytes(samples * 6)
    chunks = (b"bext" + (602).to_bytes(4, "little") + bytes(bext) +
              b"fmt " + (16).to_bytes(4, "little") +
              (1).to_bytes(2, "little") + (2).to_bytes(2, "little") +
              rate.to_bytes(4, "little") + (rate * 6).to_bytes(4, "little") +
              (6).to_bytes(2, "little") + (24).to_bytes(2, "little") +
              b"data" + len(data).to_bytes(4, "little") + data)
    path.write_bytes(b"RIFF" + (len(chunks) + 4).to_bytes(4, "little") +
                     b"WAVE" + chunks)


@unittest.skipUnless(BINARY.exists(), "build/cutmachine is not available")
class PlaceStemTests(unittest.TestCase):
    """SN4 -- a stem ingested and laid by the engine where its BWF timecode
    says, through place_sound_stem; one undo takes it away."""

    def setUp(self) -> None:
        # Never into the person's project archive, even outside CTest.
        patcher = mock.patch.dict("os.environ", {
            "CUTMACHINE_PROJECT_ARCHIVE": "off", "CUTMACHINE_LOG": "0"})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.binary = CutmachineBinary(BINARY)
        self.package = Path(self.directory.name) / "Son.cutmachine-project"
        self.binary._checked(["--create-project", str(self.package), "Son",
                              "--format", "320x180@25"])
        (self.package / "Sound").mkdir()
        self.stem = self.package / "Sound" / "stem.wav"
        # 2 s at 48 kHz, rendered from 01:00:00:00 at 25 fps.
        write_stem(self.stem, rate=48000, samples=96000, reference=90000 * 1920)

    def test_ingested_and_laid_on_the_first_frame(self) -> None:
        before = (self.package / "project.cutmachine.json").read_bytes()
        placed = place_stem(self.binary, self.package, self.stem)
        self.assertEqual(placed["first_frame"], 0)
        self.assertEqual(placed["frames"], 50)
        self.assertTrue(placed["track_added"])
        self.assertEqual(placed["not_laid"], {"value": 0, "rate": 48000})
        library = self.binary.library(self.package)
        self.assertEqual([m["filename"] for m in library], ["stem.wav"])
        self.assertEqual(placed["media_id"], library[0]["id"])
        self.binary.call_tool(self.package, "undo", {})
        after = json.loads((self.package / "project.cutmachine.json").read_text())
        tracks = after["timelines"][0]["tracks"]
        self.assertEqual(len(tracks), 2, "the added track went with the undo")
        self.assertEqual(json.loads(before)["timelines"][0]["tracks"], tracks)

    def test_a_stem_for_another_start_is_refused(self) -> None:
        with self.assertRaisesRegex(SoundError, "frames before the timeline's first frame"):
            place_stem(self.binary, self.package, self.stem, start_timecode="02:00:00:00")
        with self.assertRaisesRegex(SoundError, "needs the project's package"):
            place_stem(self.binary, self.stem, self.stem)


class SoundRendererWithoutNodeTests(unittest.TestCase):
    def test_missing_node_is_said(self) -> None:
        with mock.patch.dict("os.environ", {"CUTMACHINE_NODE": ""}), \
                mock.patch("shutil.which", return_value=None):
            renderer = SoundRenderer()
        with tempfile.TemporaryDirectory() as directory:
            ditty = Path(directory) / "x.js"
            ditty.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(SoundError, "Node 18 or later is needed"):
                renderer.render(ditty, directory)


if __name__ == "__main__":
    unittest.main()
