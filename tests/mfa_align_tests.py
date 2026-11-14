#!/usr/bin/env python3
"""ITW9 -- docs/roadmap/ITW-2026-09.md. What scripts/mfa_align.py hands the
aligner for a French, English or Spanish rush, and which language it picks,
without MFA: the words a model is given decide where it puts the boundaries,
and a word spelt in another language than the one spoken drags its
neighbours. ALIGNEMENT-2026-11 -- and how MFA's words come back onto the
transcript's tokens."""

import importlib.util
import json
import re
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "mfa_align", ROOT / "scripts" / "mfa_align.py")
ALIGN = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(ALIGN)


class FrenchIsUnchanged(unittest.TestCase):
    # What the script handed NeMo for these tokens before ITW9, recorded on
    # main at b714b33: a French rush must be aligned on the same text, or its
    # cache file (named after the text's hash) would change.
    BEFORE = {
        "84": ["quatre-vingt-quatre"],
        "22": ["vingt-deux"],
        "71": ["soixante-et-onze"],
        "80": ["quatre-vingts"],
        "99": ["quatre-vingt-dix-neuf"],
        "2027": ["deux", "mille", "vingt-sept"],
        "1985": ["mille", "neuf", "cent", "quatre-vingt-cinq"],
        "200": ["deux", "cents"],
        "1er": ["premier"],
        "2e": ["deuxième"],
        "aujourd'hui": ["aujourd'hui"],
        "peut-être": ["peut-être"],
        "Œuvre": ["œuvre"],
        "euhhh": ["euh"],
        "3D": ["trois", "d"],
        "84h": ["quatre-vingt-quatre", "h"],
        "c’est": ["c'est"],
        "–": [],
    }

    def test_the_default_language_is_french(self):
        for token, words in self.BEFORE.items():
            self.assertEqual(ALIGN.spoken(token), words, token)

    def test_naming_french_changes_nothing(self):
        for token, words in self.BEFORE.items():
            self.assertEqual(ALIGN.spoken(token, "fr"), words, token)


class English(unittest.TestCase):
    def test_numbers_are_said_in_english(self):
        self.assertEqual(ALIGN.spoken("84", "en"), ["eighty", "four"])
        self.assertEqual(ALIGN.spoken("36", "en"), ["thirty", "six"])
        self.assertEqual(ALIGN.spoken("40", "en"), ["forty"])
        self.assertEqual(ALIGN.spoken("115", "en"),
                         ["one", "hundred", "fifteen"])
        self.assertEqual(ALIGN.spoken("3000", "en"), ["three", "thousand"])

    def test_a_year_is_said_in_pairs(self):
        self.assertEqual(ALIGN.spoken("2027", "en"),
                         ["twenty", "twenty", "seven"])
        self.assertEqual(ALIGN.spoken("1985", "en"),
                         ["nineteen", "eighty", "five"])
        self.assertEqual(ALIGN.spoken("1905", "en"),
                         ["nineteen", "oh", "five"])
        self.assertEqual(ALIGN.spoken("1900", "en"), ["nineteen", "hundred"])
        self.assertEqual(ALIGN.spoken("2005", "en"),
                         ["two", "thousand", "five"])

    def test_ordinals(self):
        self.assertEqual(ALIGN.spoken("1st", "en"), ["first"])
        self.assertEqual(ALIGN.spoken("3rd", "en"), ["third"])
        self.assertEqual(ALIGN.spoken("22nd", "en"), ["twenty", "second"])
        self.assertEqual(ALIGN.spoken("20th", "en"), ["twentieth"])
        self.assertEqual(ALIGN.spoken("12th", "en"), ["twelfth"])

    def test_a_hyphen_separates_two_spoken_words(self):
        # The English text has no "-": joined, "eighty-four" was one word
        # the aligner had never heard.
        self.assertEqual(ALIGN.spoken("84-hour", "en"),
                         ["eighty", "four", "hour"])
        self.assertEqual(ALIGN.spoken("well-known", "en"), ["well", "known"])

    def test_letters_outside_the_vocabulary_lose_their_accent(self):
        self.assertEqual(ALIGN.spoken("Château", "en"), ["chateau"])
        self.assertEqual(ALIGN.spoken("Île", "en"), ["ile"])
        self.assertEqual(ALIGN.spoken("ORIA’s", "en"), ["oria's"])


class Spanish(unittest.TestCase):
    def test_numbers_are_said_in_spanish(self):
        self.assertEqual(ALIGN.spoken("84", "es"), ["ochenta", "y", "cuatro"])
        self.assertEqual(ALIGN.spoken("22", "es"), ["veintidós"])
        self.assertEqual(ALIGN.spoken("40", "es"), ["cuarenta"])
        self.assertEqual(ALIGN.spoken("100", "es"), ["cien"])
        self.assertEqual(ALIGN.spoken("101", "es"), ["ciento", "uno"])
        self.assertEqual(ALIGN.spoken("500", "es"), ["quinientos"])
        self.assertEqual(ALIGN.spoken("2027", "es"),
                         ["dos", "mil", "veintisiete"])

    def test_ordinals_take_the_gender_of_their_suffix(self):
        # NFKC has turned º and ª into o and a before tokens are read.
        self.assertEqual(ALIGN.spoken("1o", "es"), ["primero"])
        self.assertEqual(ALIGN.spoken("1a", "es"), ["primera"])
        self.assertEqual(ALIGN.spoken("3er", "es"), ["tercer"])
        self.assertEqual(ALIGN.spoken("12o", "es"), ["doce"])

    def test_the_spanish_vocabulary(self):
        self.assertEqual(ALIGN.spoken("París", "es"), ["parís"])
        self.assertEqual(ALIGN.spoken("pingüino", "es"), ["pingüino"])
        self.assertEqual(ALIGN.spoken("Ñandú", "es"), ["ñandú"])
        # Spanish text is spelt with neither ' nor -.
        self.assertEqual(ALIGN.spoken("d'Artagnan", "es"), ["dartagnan"])
        self.assertEqual(ALIGN.spoken("Saint-Malo", "es"),
                         ["saint", "malo"])
        self.assertEqual(ALIGN.spoken("Île", "es"), ["ile"])


class Fillers(unittest.TestCase):
    def quantized(self, text, language):
        entries = [(text, Decimal("0.0"), Decimal("0.4"))]
        return ALIGN.quantize(entries, (25, 1), 100, language)[0]

    def test_each_language_has_its_own(self):
        self.assertTrue(self.quantized("euh", "fr")["filler_word"])
        self.assertTrue(self.quantized("Umm", "en")["filler_word"])
        self.assertTrue(self.quantized("eh", "es")["filler_word"])
        self.assertFalse(self.quantized("um", "fr")["filler_word"])
        self.assertFalse(self.quantized("euh", "en")["filler_word"])


def transcript(language, text, detected=None):
    result = {"language": language,
              "words": [{"text": word} for word in text.split()]}
    if detected is not None:
        result["detected_language"] = detected
    return result


class Language(unittest.TestCase):
    ENGLISH = "Can you see the boats from the pier? By noon, we will"
    SPANISH = "¿Vas al puerto con el barco de tu padre? Es muy pequeño"
    FRENCH = "Le chat dort sur le toit et la pluie tombe dans la cour"

    def test_a_forced_language_wins(self):
        self.assertEqual(ALIGN.alignment_language(transcript("es", self.ENGLISH)),
                         ("es", "transcript"))

    def test_then_whisper_s_detection(self):
        self.assertEqual(
            ALIGN.alignment_language(transcript("auto", self.FRENCH, "en")),
            ("en", "detected"))

    def test_then_the_words(self):
        self.assertEqual(ALIGN.alignment_language(transcript("auto", self.ENGLISH)),
                         ("en", "text"))
        self.assertEqual(ALIGN.alignment_language(transcript("auto", self.SPANISH)),
                         ("es", "text"))
        self.assertEqual(ALIGN.alignment_language(transcript("auto", self.FRENCH)),
                         ("fr", "text"))

    def test_too_few_words_stay_french(self):
        self.assertEqual(ALIGN.guess_language("Okay."), "fr")
        self.assertEqual(ALIGN.guess_language(""), "fr")
        # One English word against one Spanish one decides nothing.
        self.assertEqual(ALIGN.guess_language("the el"), "fr")

    def test_a_language_without_a_model_is_named_not_mapped(self):
        self.assertEqual(ALIGN.alignment_language(transcript("de", "Hallo")),
                         ("de", "transcript"))


class TokenSpans(unittest.TestCase):
    def words(self, *labels):
        return [(label, Decimal(str(index)), Decimal(str(index)) + 1)
                for index, label in enumerate(labels)]

    def test_a_word_mfa_split_spans_both_halves(self):
        tokens = [("centre-ville.", ["centre-ville"]), ("On", ["on"])]
        entries = ALIGN.token_spans(
            tokens, self.words("centre", "ville", "on"))
        self.assertEqual(entries, [("centre-ville.", 0, 2), ("On", 2, 3)])

    def test_a_number_said_in_words_spans_them(self):
        tokens = [("84", ["quatre-vingt-quatre"]), ("ans", ["ans"])]
        entries = ALIGN.token_spans(
            tokens, self.words("quatre", "vingt", "quatre", "ans"))
        self.assertEqual(entries, [("84", 0, 3), ("ans", 3, 4)])

    def test_a_token_mfa_dropped_joins_the_previous_one(self):
        text = "le vent a soufflé toute la nuit sur les toits"
        tokens = [(word, [word]) for word in text.split()]
        entries = ALIGN.token_spans(
            tokens, self.words(*text.replace(" a ", " ").split()))
        self.assertEqual([entry[0] for entry in entries][:3],
                         ["le", "vent a", "soufflé"])

    def test_other_words_are_refused(self):
        tokens = [("bonjour", ["bonjour"]), ("à", ["à"]), ("tous", ["tous"])]
        self.assertIsNone(ALIGN.token_spans(
            tokens, self.words("rien", "de", "commun")))


class LongRushes(unittest.TestCase):
    def test_silences_are_read_on_the_engine_s_voice_line(self):
        import array
        import math
        import wave
        rate = 16000
        tone = [int(8000 * math.sin(2 * math.pi * 220 * n / rate))
                for n in range(rate)]
        samples = array.array("h", tone + [0] * (rate // 2) + tone)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rush.wav"
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(2)
                handle.setframerate(rate)
                handle.writeframes(samples.tobytes())
            self.assertEqual(ALIGN.silences(path), [(1.0, 1.5)])

    def test_a_short_rush_or_a_text_without_times_goes_whole(self):
        self.assertIsNone(ALIGN.utterance_segments(80.0, [(0.0, 1.0)], []))
        self.assertIsNone(
            ALIGN.utterance_segments(200.0, [(0.0, 1.0), None], [(50, 51)]))

    def test_cuts_fall_in_silences_no_word_overlaps(self):
        times = [(10.0, 11.0), (34.0, 36.0), (40.0, 41.0), (70.0, 71.0),
                 (100.0, 101.0)]
        quiet = [(20.0, 21.0), (35.0, 35.5), (38.0, 39.0), (80.0, 81.0)]
        segments = ALIGN.utterance_segments(120.0, times, quiet)
        # 20.5 is too early, 35.25 lies inside a word: the first cut is at
        # 38.5, the next one 30 s on at least.
        self.assertEqual(segments, [(0.0, 38.5, [0, 1]),
                                    (38.5, 80.5, [2, 3]),
                                    (80.5, 120.0, [4])])

    def test_the_textgrid_holds_each_utterance_s_spoken_words(self):
        tokens = [("Bonjour", ["bonjour"]), ("84", ["quatre-vingt-quatre"])]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rush.TextGrid"
            ALIGN.write_textgrid(path, 100.0,
                                 [(0.0, 50.0, [0]), (50.0, 100.0, [1])],
                                 tokens)
            text = path.read_text(encoding="utf-8")
        self.assertIn("intervals: size = 2", text)
        self.assertIn('text = "bonjour"', text)
        self.assertIn('text = "quatre-vingt-quatre"', text)
        self.assertIn("xmax = 100.0", text)


class Run(unittest.TestCase):
    """main() over a fake package, with MFA and FFmpeg replaced: one MFA run
    per language, each with its models, a rush in a language without one
    skipped and named, and a rush MFA left unaligned skipped."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.package = root / "Film.cutmachine-project"
        cache = self.package / ".cutmachine" / "transcripts"
        cache.mkdir(parents=True)
        library = []
        for media_id, language, text in [
                ("01ENGLISH", "auto", "By noon you will have the 84 pages"),
                ("01SPANISH", "es", "Tendrás 84 horas"),
                ("01FRENCH", "fr", "Le centre-ville"),
                ("01LONG", "fr", "Bonjour à tous et merci"),
                ("01GERMAN", "de", "Guten Tag")]:
            seconds = 200 if media_id == "01LONG" else 10
            library.append({"id": media_id, "filename": f"{media_id}.MP4",
                            "path": f"{media_id}.MP4",
                            "rate": {"num": 25, "den": 1},
                            "duration": {"value": 25 * seconds, "rate": 25}})
            step = seconds / len(text.split())
            words = [{"text": word,
                      "start": {"value": round(index * step * 1000),
                                "rate": 1000},
                      "end": {"value": round((index + 0.5) * step * 1000),
                              "rate": 1000}}
                     for index, word in enumerate(text.split())]
            (cache / f"{media_id}.json").write_text(json.dumps(
                {"language": language, "whisper_model": "ggml-large-v3.bin",
                 "verbatim": True, "words": words}), encoding="utf-8")
            (self.package / f"{media_id}.MP4").write_bytes(b"")
        self.project = self.package / "project.cutmachine.json"
        self.project.write_text(json.dumps(
            {"project_version": 3, "library": library}), encoding="utf-8")
        self.models = root / "models"
        self.models.mkdir()
        for files in ALIGN.MODELS.values():
            for kind in ("acoustic", "dictionary", "g2p"):
                (self.models / files[kind]).write_bytes(b"")
        self.work = root / "work"
        self.report = root / "report.json"
        self.runs = []
        self.unaligned = set()
        self.unaligned_whole = set()

    def tearDown(self):
        self.directory.cleanup()

    def fake_run(self, command, **_):
        if command[0] == "ffmpeg":
            Path(command[-1]).write_bytes(b"")
            return mock.Mock(returncode=0)
        if command[1] == "version":
            return mock.Mock(returncode=0, stdout="3.4.3\n")
        corpus, acoustic, output = Path(command[2]), command[4], Path(command[5])
        self.runs.append(Path(acoustic).name +
                         (" segmented" if "segmented" in str(corpus) else ""))
        output.mkdir(parents=True)
        rows = ["file,begin,end,speaker,overall_log_likelihood"]
        inputs = sorted(corpus.glob("*.lab")) + sorted(corpus.glob("*.TextGrid"))
        for lab in inputs:
            stem = lab.stem
            text = lab.read_text(encoding="utf-8")
            if lab.suffix == ".TextGrid":
                text = " ".join(re.findall(r'text = "(.*)"', text))
            if stem in self.unaligned or (
                    stem in self.unaligned_whole and lab.suffix == ".lab"):
                rows.append(f"{stem},0.0,10.0,corpus,")
                continue
            rows.append(f"{stem},0.0,10.0,corpus,-45.0")
            # MFA splits what it was given at hyphens.
            labels = text.replace("-", " ").split()
            entries = [[0.3 * index, 0.3 * index + 0.25, label]
                       for index, label in enumerate(labels)]
            (output / f"{stem}.json").write_text(json.dumps(
                {"tiers": {"words": {"type": "interval",
                                     "entries": entries}}}),
                encoding="utf-8")
        (output / "alignment_analysis.csv").write_text(
            "\n".join(rows) + "\n", encoding="utf-8")
        return mock.Mock(returncode=0)

    def run_main(self, *media):
        with mock.patch.object(ALIGN.subprocess, "run", self.fake_run):
            return ALIGN.main([str(self.project), *media,
                               "--work", str(self.work),
                               "--report", str(self.report),
                               "--mfa", "mfa", "--models", str(self.models)])

    def test_one_run_per_language(self):
        self.assertEqual(self.run_main("01ENGLISH", "01SPANISH", "01GERMAN"), 0)
        self.assertEqual(self.runs, ["acoustic-english_mfa-v3.1.0.zip",
                                     "acoustic-spanish_mfa-v3.3.0.zip"])
        report = json.loads(self.report.read_text(encoding="utf-8"))
        aligned = {entry["media_id"]: entry for entry in report["aligned"]}
        self.assertEqual(aligned["01ENGLISH"]["language"], "en")
        self.assertEqual(aligned["01ENGLISH"]["language_source"], "text")
        self.assertEqual(aligned["01SPANISH"]["model"], "spanish_mfa")
        self.assertEqual(report["skipped"], [{
            "media_id": "01GERMAN", "reason": "unsupported_language",
            "language": "de"}])
        self.assertIn("mfa_ms", report)
        cache = json.loads(Path(aligned["01SPANISH"]["path"]).read_text(
            encoding="utf-8"))
        self.assertEqual(cache["language"], "es")
        self.assertEqual(cache["alignment"]["method"], "mfa")
        self.assertEqual(cache["alignment"]["model"], "spanish_mfa")
        self.assertEqual(cache["alignment"]["version"], "MFA 3.4.3")
        self.assertTrue(Path(aligned["01SPANISH"]["path"]).name.startswith(
            "mfa-spanish_mfa-"))
        # The surface words stay Whisper's; MFA was handed them spoken.
        self.assertEqual([word["text"] for word in cache["words"]],
                         ["Tendrás", "84", "horas"])
        lab = self.work / "mfa-es" / "corpus" / "01SPANISH.lab"
        self.assertEqual(lab.read_text(encoding="utf-8").strip(),
                         "tendrás ochenta y cuatro horas")

    def test_a_split_word_keeps_its_surface(self):
        self.assertEqual(self.run_main("01FRENCH"), 0)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        cache = json.loads(Path(report["aligned"][0]["path"]).read_text(
            encoding="utf-8"))
        self.assertEqual([word["text"] for word in cache["words"]],
                         ["Le", "centre-ville"])

    def test_a_rush_mfa_left_unaligned_is_skipped(self):
        self.unaligned = {"01SPANISH"}
        self.assertEqual(self.run_main("01SPANISH", "01FRENCH"), 0)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual([entry["media_id"] for entry in report["aligned"]],
                         ["01FRENCH"])
        self.assertEqual(report["skipped"], [{
            "media_id": "01SPANISH", "reason": "not_aligned"}])

    def test_a_long_rush_refused_whole_is_cut_at_its_silences(self):
        self.unaligned_whole = {"01LONG"}
        with mock.patch.object(ALIGN, "silences",
                               return_value=[(90.0, 91.0), (150.0, 151.0)]):
            self.assertEqual(self.run_main("01LONG", "01FRENCH"), 0)
        self.assertEqual(self.runs, ["acoustic-french_mfa-v3.0.0.zip",
                                     "acoustic-french_mfa-v3.0.0.zip "
                                     "segmented"])
        report = json.loads(self.report.read_text(encoding="utf-8"))
        aligned = {entry["media_id"]: entry for entry in report["aligned"]}
        self.assertTrue(aligned["01LONG"]["segmented"])
        self.assertFalse(aligned["01FRENCH"]["segmented"])
        self.assertEqual(report["skipped"], [])
        grid = (self.work / "mfa-fr-segmented" / "corpus" /
                "01LONG.TextGrid").read_text(encoding="utf-8")
        self.assertIn("intervals: size = 2", grid)

    def test_a_short_rush_refused_whole_is_not_cut(self):
        self.unaligned_whole = {"01FRENCH"}
        self.assertEqual(self.run_main("01FRENCH"), 0)
        self.assertEqual(self.runs, ["acoustic-french_mfa-v3.0.0.zip"])
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["skipped"], [{
            "media_id": "01FRENCH", "reason": "not_aligned"}])

    def test_missing_models_are_named(self):
        (self.models / ALIGN.MODELS["fr"]["g2p"]).unlink()
        self.assertEqual(self.run_main("01FRENCH"), 2)
        self.assertEqual(self.runs, [])


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
