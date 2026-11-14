"""FI1 -- ROADMAP.md: a failed pack never discards its previous output."""

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "motion_web_pack", ROOT / "tools/motion-web-import/pack.py")
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)


class PackTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="cutmachine-pack-test-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name).resolve()
        self.pieces = self.root / "pieces"
        self.pieces.mkdir()
        self.out = self.root / "library"
        self.out.mkdir()
        self.sentinel = self.out / "previous.json"
        self.sentinel.write_text('{"preserve":true}\n')
        self.classes = self.root / "classes.json"
        self.classes.write_text('{}')
        self.entry = {"ok": True, "name": "hf-test", "source": "test",
                      "stats": {"nodes": 1}}
        (self.pieces / "hf-test.json").write_text(json.dumps({
            "origin": "test", "composition": {"root": {"type": "fill"}}}))
        self.report([self.entry])

    def report(self, entries):
        (self.pieces / "import-report.json").write_text(json.dumps(entries))

    def run_pack(self, out=None):
        return PACK.pack(self.pieces, self.classes, out or self.out)

    def assert_previous(self):
        self.assertEqual(self.sentinel.read_text(), '{"preserve":true}\n')

    def test_success_replaces_only_the_output(self):
        self.assertEqual(self.run_pack(), {"prête": 1})
        self.assertTrue((self.out / "hf-test.json").is_file())
        self.assertTrue((self.out / "LISEZMOI.md").is_file())
        self.assertFalse(self.sentinel.exists())
        self.assertTrue((self.pieces / "hf-test.json").is_file())
        self.assertEqual(list(self.root.glob(".library.*")), [])

    def test_missing_late_piece_preserves_the_previous_pack(self):
        self.report([self.entry, {**self.entry, "name": "hf-missing"}])
        with self.assertRaises(FileNotFoundError):
            self.run_pack()
        self.assert_previous()
        self.assertFalse((self.out / "hf-test.json").exists())

    def test_publication_failure_restores_the_previous_pack(self):
        replace = os.replace

        def fail_publication(source, destination):
            if Path(source).name == "library" and Path(source).parent != self.root:
                raise OSError("injected publication failure")
            return replace(source, destination)

        with patch.object(PACK.os, "replace", side_effect=fail_publication):
            with self.assertRaisesRegex(OSError, "injected"):
                self.run_pack()
        self.assert_previous()

    def test_failed_restore_keeps_a_recoverable_backup(self):
        replace = os.replace

        def fail_after_backup(source, destination):
            if Path(source) != self.out:
                raise OSError("injected failure")
            return replace(source, destination)

        with patch.object(PACK.os, "replace", side_effect=fail_after_backup):
            with self.assertRaises(OSError):
                self.run_pack()
        backups = list(self.root.glob(".library.previous-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / "previous.json").read_text(),
                         '{"preserve":true}\n')

    def test_output_must_not_replace_an_input(self):
        for out in (self.pieces, self.root):
            with self.subTest(out=out), self.assertRaises(ValueError):
                self.run_pack(out)
        self.assert_previous()
        self.assertTrue((self.pieces / "hf-test.json").is_file())

    def test_output_symlink_does_not_touch_its_target(self):
        link = self.root / "link"
        link.symlink_to(self.out, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.run_pack(link)
        self.assert_previous()

    def test_piece_names_cannot_escape_the_pack(self):
        self.report([{**self.entry, "name": "../escape"}])
        with self.assertRaises(ValueError):
            self.run_pack()
        self.assert_previous()

    def test_no_validated_piece_cannot_erase_the_previous_pack(self):
        self.report([{**self.entry, "ok": None}])
        with self.assertRaisesRegex(ValueError, "aucune pièce validée"):
            self.run_pack()
        self.assert_previous()


if __name__ == "__main__":
    unittest.main()
