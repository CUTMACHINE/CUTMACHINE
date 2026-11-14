#!/usr/bin/env python3

import importlib.util
import json
import struct
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "word-alignment-benchmark" / "benchmark.py"
SPEC = importlib.util.spec_from_file_location("word_alignment_benchmark", MODULE_PATH)
BENCHMARK = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(BENCHMARK)
CPROM_SPEC = importlib.util.spec_from_file_location(
    "word_alignment_cprom", MODULE_PATH.parent / "cprom.py"
)
CPROM = importlib.util.module_from_spec(CPROM_SPEC)
assert CPROM_SPEC.loader is not None
CPROM_SPEC.loader.exec_module(CPROM)


def word(text, start, end, silence=0):
    return {
        "text": text,
        "frame_start": start,
        "frame_end": end,
        "confidence": None,
        "filler_word": False,
        "silence_after_frames": silence,
        "silence_after_ms": silence * 40,
        "speaker_id": None,
    }


def example_manifest(method, words, wall_time_ms=0):
    return {
        "schema_version": 1,
        "sample_id": "sample-1",
        "media_id": "01K00000000000000000000000",
        "language": "fr",
        "source_rate": {"num": 25, "den": 1},
        "alignment": {
            "method": method,
            "version": "test",
            "model": "fixture",
            "timing_provenance": "fixture",
            "reference_text_sha256": "a" * 64,
        },
        "processing": {
            "wall_time_ms": wall_time_ms,
            "audio_duration_frames": 50,
        },
        "words": words,
    }


class WordAlignmentBenchmarkTests(unittest.TestCase):
    def test_quantization_is_outward_and_marks_fillers(self):
        words = BENCHMARK.quantize_words(
            [
                ("euh", Decimal("0.10"), Decimal("0.20")),
                ("bonjour", Decimal("0.30"), Decimal("0.50")),
            ],
            (30000, 1001),
        )
        self.assertEqual((words[0]["frame_start"], words[0]["frame_end"]), (2, 6))
        self.assertEqual(words[0]["silence_after_frames"], 2)
        self.assertEqual(words[0]["silence_after_ms"], 67)
        self.assertTrue(words[0]["filler_word"])

    def test_cutmachine_time_must_lie_on_source_frame_grid(self):
        self.assertEqual(
            BENCHMARK.rational_time_to_frame(
                {"value": 1001, "rate": 30000}, (30000, 1001), "start"
            ),
            1,
        )
        with self.assertRaisesRegex(BENCHMARK.BenchmarkError, "not exactly"):
            BENCHMARK.rational_time_to_frame(
                {"value": 1, "rate": 1000}, (25, 1), "start"
            )

    def test_lcs_ignores_punctuation_and_skips_insertions(self):
        gold = [word("Bonjour,", 0, 5, 1), word("monde", 6, 10)]
        predicted = [
            word("bonjour", 0, 5),
            word("vraiment", 5, 6),
            word("monde!", 6, 10),
        ]
        self.assertEqual(
            BENCHMARK.matched_word_indices(gold, predicted), [(0, 0), (1, 2)]
        )

    def test_nemo_ctm_ignores_optional_lexicon_columns(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctm = Path(temporary) / "words.ctm"
            ctm.write_text(
                "sample 1 0.16 0.24 Notre NA lex NA\n", encoding="utf-8"
            )
            self.assertEqual(
                BENCHMARK.ctm_entries(ctm),
                [("Notre", Decimal("0.16"), Decimal("0.40"))],
            )

    def test_score_reports_boundary_error_and_runtime(self):
        gold = example_manifest(
            "gold", [word("bonjour", 0, 5, 1), word("monde", 6, 10)]
        )
        predicted = example_manifest(
            "nemo", [word("bonjour", 1, 5, 1), word("monde", 6, 12)], 2000
        )
        score = BENCHMARK.score_pair(gold, predicted)
        self.assertEqual(score["matched_words"], 2)
        self.assertAlmostEqual(score["boundary_mae_ms"], 30.0)
        self.assertAlmostEqual(score["within_one_frame_percent"], 50.0)
        self.assertAlmostEqual(score["processing_seconds_per_audio_minute"], 60.0)

    def test_agreement_does_not_claim_accuracy_or_a_winner(self):
        left = example_manifest("cutmachine", [word("bonjour", 0, 5)])
        right = example_manifest("nemo", [word("bonjour", 2, 6)])
        result = BENCHMARK.agreement_pair(left, right)
        self.assertEqual(result["status"], "agreement_not_accuracy")
        self.assertAlmostEqual(
            result["boundary_mean_absolute_disagreement_ms"], 60.0
        )
        self.assertIsNone(result["accuracy"])
        self.assertIsNone(result["winner"])

    def test_agreement_set_aggregates_without_declaring_a_winner(self):
        left = example_manifest("cutmachine", [word("bonjour", 0, 5)])
        right = example_manifest("nemo", [word("bonjour", 2, 6)])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            BENCHMARK.write_json(root / "left" / "sample.json", left)
            BENCHMARK.write_json(root / "right" / "sample.json", right)
            result = BENCHMARK.agreement_directories(
                root / "left", root / "right"
            )
        self.assertEqual(result["matched_words"], 1)
        self.assertAlmostEqual(
            result["boundary_mean_absolute_disagreement_ms"], 60.0
        )
        self.assertIsNone(result["winner"])

    def test_score_resolve_reports_wer_and_only_native_word_boundaries(self):
        gold = example_manifest(
            "gold",
            [
                word("un", 0, 3, 1),
                word("deux", 4, 6, 1),
                word("trois", 7, 10),
            ],
        )
        capture = {
            "resolve_version": "20.3.1.6",
            "language": "fr",
            "source_rate": {"num": 25, "den": 1},
            "audio_duration_frames": 50,
            "wall_time_ms": 1000,
            "settings": {"chars_per_line": 1},
            "cues": [
                {"text": "un", "frame_start": 1, "frame_end": 3},
                {"text": "deux quatre", "frame_start": 4, "frame_end": 10},
            ],
        }
        score = BENCHMARK.score_resolve_captions(gold, capture)
        self.assertAlmostEqual(score["word_error_rate_percent"], 100 / 3)
        self.assertEqual(score["correct_words"], 2)
        self.assertEqual(score["substitutions"], 1)
        self.assertEqual(score["native_single_word_cues"], 1)
        timing = score["native_single_word_timing"]
        self.assertEqual(timing["matched_words"], 1)
        self.assertAlmostEqual(timing["boundary_mae_ms"], 20.0)
        self.assertAlmostEqual(timing["within_one_frame_percent"], 100.0)

    def test_ensemble_expands_and_reconciles_boundaries(self):
        first = example_manifest(
            "nemo", [word("un", 2, 5, 1), word("deux", 6, 10)]
        )
        second = example_manifest(
            "nemo", [word("un", 4, 7), word("deux", 7, 12)]
        )
        result = BENCHMARK.ensemble_manifests([first, second], 1234)
        self.assertEqual(
            [
                (entry["frame_start"], entry["frame_end"])
                for entry in result["words"]
            ],
            [(2, 6), (6, 12)],
        )
        self.assertEqual(result["processing"]["wall_time_ms"], 1234)
        self.assertIn("expand-1-frame", result["alignment"]["timing_provenance"])
        self.assertIn(
            "00:00:00,080 --> 00:00:00,240",
            BENCHMARK.srt_for_manifest(result),
        )

    def test_ensemble_refuses_different_words(self):
        first = example_manifest("nemo", [word("un", 2, 5)])
        second = example_manifest("nemo", [word("deux", 2, 5)])
        with self.assertRaisesRegex(BENCHMARK.BenchmarkError, "words\\[0\\]"):
            BENCHMARK.ensemble_manifests([first, second], 0)

    def test_directory_score_refuses_duplicate_predictions(self):
        gold = example_manifest("gold", [word("bonjour", 0, 5)])
        prediction = example_manifest("mfa", [word("bonjour", 0, 5)])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            BENCHMARK.write_json(root / "gold" / "sample.json", gold)
            BENCHMARK.write_json(root / "predictions" / "a.json", prediction)
            BENCHMARK.write_json(root / "predictions" / "b.json", prediction)
            with self.assertRaisesRegex(BENCHMARK.BenchmarkError, "duplicate"):
                BENCHMARK.score_directories(root / "gold", root / "predictions")

    def test_manifest_rejects_overlapping_words(self):
        value = example_manifest(
            "gold", [word("un", 0, 5), word("deux", 4, 8)]
        )
        with self.assertRaisesRegex(BENCHMARK.BenchmarkError, "overlaps"):
            BENCHMARK.validate_manifest(value)


    def test_qwen_ctm_becomes_a_qwen_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "words.ctm").write_text(
                "s 1 0.160 0.080 bonjour\ns 1 0.320 0.000 à\n"
                "s 1 0.320 0.160 vous\n",
                encoding="utf-8",
            )
            (root / "text.txt").write_text("bonjour à vous", encoding="utf-8")
            code = BENCHMARK.main(
                [
                    "normalize-qwen", "--input", str(root / "words.ctm"),
                    "--output", str(root / "out.json"), "--sample-id", "s",
                    "--media-id", "01K00000000000000000000000",
                    "--source-rate", "25/1", "--version", "test",
                    "--model", "fixture", "--reference-text",
                    str(root / "text.txt"), "--wall-time-ms", "1",
                    "--audio-duration-frames", "20",
                ]
            )
            self.assertEqual(code, 0)
            value = BENCHMARK.load_json(root / "out.json")
        self.assertEqual(value["alignment"]["method"], "qwen")
        # A word the aligner gave no duration joins the next one: no word
        # may own zero frames.
        self.assertEqual(
            [(w["text"], w["frame_start"], w["frame_end"]) for w in value["words"]],
            [("bonjour", 4, 6), ("à vous", 8, 12)],
        )

    def test_pause_runs_need_voice_on_both_sides(self):
        voiced = [True, True, False, False, False, True, False, False]
        self.assertEqual(BENCHMARK.pause_runs(voiced, 0, 8, 3), [(2, 5)])
        self.assertEqual(BENCHMARK.pause_runs(voiced, 0, 8, 4), [])

    def test_envelope_junction_measures_both_aligners_on_one_edge(self):
        voiced = [True] * 10 + [False] * 6 + [True] * 10
        left = example_manifest(
            "nemo", [word("bonjour", 2, 12, 3), word("monde", 15, 24)]
        )
        right = example_manifest(
            "qwen", [word("bonjour", 2, 10, 6), word("monde", 16, 24)]
        )
        result = BENCHMARK.envelope_junctions(left, right, voiced)
        self.assertEqual(result["pause_junctions"], 1)
        self.assertEqual(result["errors_frames"]["left"], {"end": [2], "start": [-1]})
        self.assertEqual(result["errors_frames"]["right"], {"end": [0], "start": [0]})

    def test_envelope_junction_ignores_a_neighbours_pause(self):
        # The pause belongs before « des »; « des | volumes » must not read it.
        voiced = [True] * 10 + [False] * 6 + [True] * 14
        words = [
            word("construire", 0, 10, 6),
            word("des", 16, 20),
            word("volumes", 20, 30),
        ]
        result = BENCHMARK.envelope_junctions(
            example_manifest("nemo", words), example_manifest("qwen", words), voiced
        )
        self.assertEqual(result["pause_junctions"], 1)
        self.assertEqual(result["continuous_junctions"], 1)

    def test_junction_kit_names_words_and_never_frames(self):
        voiced = [True] * 10 + [False] * 6 + [True] * 34
        base = example_manifest(
            "nemo",
            [word("bonjour", 0, 10, 6), word("à", 16, 18), word("vous", 18, 30)],
        )
        kit = BENCHMARK.junction_kit(base, [], voiced, 5)
        self.assertEqual(
            kit["junctions"],
            [
                {
                    "id": "J001",
                    "before_index": 0,
                    "after_index": 1,
                    "before": "bonjour",
                    "after": "à",
                }
            ],
        )
        self.assertNotIn("frame", json.dumps(kit["junctions"]))
        self.assertIn("bonjour [J001] à vous", BENCHMARK.junction_sheet(kit))

    def test_junction_gold_reads_resolve_markers_and_scores(self):
        voiced = [True] * 10 + [False] * 6 + [True] * 34
        base = example_manifest(
            "nemo", [word("bonjour", 0, 10, 6), word("vous", 16, 30)]
        )
        kit = BENCHMARK.junction_kit(base, [], voiced, 5)
        edl = "\n".join(
            [
                "TITLE: t",
                "FCM: NON-DROP FRAME",
                "001  001      V     C        "
                "01:00:00:09 01:00:00:10 01:00:00:09 01:00:00:10",
                " |C:ResolveColorBlue |M:J001 F |D:1",
                "002  001      V     C        "
                "01:00:00:15 01:00:00:16 01:00:00:15 01:00:00:16",
                "* LOC: 01:00:00:15 BLUE    j1-d",
            ]
        )
        markers = BENCHMARK.edl_markers(edl, 25, 90000)
        self.assertEqual(markers, {"J001": {"F": 9, "D": 15}})
        gold = BENCHMARK.junction_gold(kit, markers, "test")
        self.assertEqual(gold["junctions"][0]["end_frame"], 10)
        self.assertEqual(gold["junctions"][0]["start_frame"], 15)
        score = BENCHMARK.score_junctions(gold, base)
        self.assertEqual(score["errors_frames"], {"end": [0], "start": [1]})
        self.assertEqual(score["both_within_one_frame_percent"], 100.0)

    def test_junction_gold_refuses_a_start_before_the_end(self):
        voiced = [True] * 10 + [False] * 6 + [True] * 34
        base = example_manifest(
            "nemo", [word("bonjour", 0, 10, 6), word("vous", 16, 30)]
        )
        kit = BENCHMARK.junction_kit(base, [], voiced, 5)
        with self.assertRaisesRegex(BENCHMARK.BenchmarkError, "D is before"):
            BENCHMARK.junction_gold(kit, {"J001": {"F": 12, "D": 12}}, "test")

    def test_envelope_junction_can_be_located_by_one_aligner(self):
        voiced = [True] * 10 + [False] * 6 + [True] * 14
        left = example_manifest(
            "nemo", [word("bonjour", 0, 10, 6), word("monde", 16, 30)]
        )
        # This aligner put « monde » before the pause: it cannot locate it.
        right = example_manifest(
            "qwen", [word("bonjour", 0, 4), word("monde", 4, 30)]
        )
        by_left = BENCHMARK.envelope_junctions(left, right, voiced, select="left")
        by_right = BENCHMARK.envelope_junctions(left, right, voiced, select="right")
        self.assertEqual(by_left["pause_junctions"], 1)
        self.assertEqual(by_left["errors_frames"]["right"]["start"], [-12])
        self.assertEqual(by_right["pause_junctions"], 0)

    def test_voice_line_is_a_ratio_of_the_speech_level_in_amplitude(self):
        # Power 10 against a speech power of 100 is an amplitude ratio of
        # 31.6 %: voiced under a 25 % line, silent under a 50 % one.
        powers = [100, 100, 100, 100, 100, 100, 100, 100, 100, 10]
        self.assertEqual(
            BENCHMARK.voiced_frames(powers, (50, 1), 10, 25)[-1], True
        )
        self.assertEqual(
            BENCHMARK.voiced_frames(powers, (50, 1), 10, 50)[-1], False
        )

    def test_unwritten_markers_are_read_with_their_duration(self):
        edl = "\n".join(
            [
                "001  001      V     C        "
                "01:00:00:09 01:00:00:10 01:00:00:09 01:00:00:10",
                " |C:ResolveColorBlue |M:J001 F |D:1",
                "002  001      V     C        "
                "01:00:00:12 01:00:00:16 01:00:00:12 01:00:00:16",
                " |C:ResolveColorRed |M:M euh |D:4",
                "003  001      V     C        "
                "01:00:00:20 01:00:00:21 01:00:00:20 01:00:00:21",
                "* LOC: 01:00:00:20 RED     m manque",
            ]
        )
        self.assertEqual(
            BENCHMARK.edl_unwritten_words(edl, 25, 90000),
            [
                {"text": "euh", "frame_start": 12, "frame_end": 16},
                {"text": "manque", "frame_start": 20, "frame_end": 21},
            ],
        )
        self.assertEqual(BENCHMARK.edl_markers(edl, 25, 90000), {"J001": {"F": 9}})

    def test_unwritten_word_is_written_only_beyond_the_reference(self):
        gold = {
            "kind": "junction_gold",
            "sample_id": "sample-1",
            "source_rate": {"num": 25, "den": 1},
            "reference_words": ["le", "manque"],
            "junctions": [],
            "unwritten_words": [
                {"text": "manque", "frame_start": 10, "frame_end": 15}
            ],
        }
        once = example_manifest(
            "cutmachine", [word("le", 0, 10), word("manque", 10, 20)]
        )
        twice = example_manifest(
            "cutmachine",
            [word("le", 0, 10), word("manque", 10, 15), word("manque", 15, 20)],
        )
        self.assertEqual(BENCHMARK.score_unwritten(gold, once, None)["written"], 0)
        self.assertEqual(BENCHMARK.score_unwritten(gold, twice, None)["written"], 1)

    def test_a_stretched_neighbour_hides_an_unwritten_word(self):
        gold = {
            "kind": "junction_gold",
            "sample_id": "sample-1",
            "source_rate": {"num": 25, "den": 1},
            "reference_words": ["porté", "sur"],
            "junctions": [],
            "unwritten_words": [{"text": "donc", "frame_start": 10, "frame_end": 14}],
        }
        voiced = [True] * 30
        holed = example_manifest(
            "nemo", [word("porté", 0, 9, 6), word("sur", 15, 30)]
        )
        stretched = example_manifest(
            "qwen", [word("porté", 0, 15), word("sur", 15, 30)]
        )
        result = BENCHMARK.score_unwritten(gold, holed, voiced)
        self.assertEqual((result["exposed"], result["holes"]), (1, 1))
        self.assertEqual(
            BENCHMARK.score_unwritten(gold, stretched, voiced)["exposed"], 0
        )
        # The engine's exit follows the voice up to the next word and
        # swallows the same hole.
        self.assertEqual(
            BENCHMARK.score_unwritten(gold, holed, voiced, True)["exposed"], 0
        )

    def test_engine_boundaries_leave_a_word_between_silences(self):
        gold = {
            "kind": "junction_gold",
            "sample_id": "sample-1",
            "source_rate": {"num": 25, "den": 1},
            "reference_words": ["porté", "sur"],
            "junctions": [],
            "unwritten_words": [{"text": "euh", "frame_start": 12, "frame_end": 17}],
        }
        voiced = [True] * 9 + [False] * 3 + [True] * 5 + [False] * 3 + [True] * 10
        holed = example_manifest(
            "nemo", [word("porté", 0, 9, 11), word("sur", 20, 30)]
        )
        self.assertEqual(
            BENCHMARK.engine_word_spans(holed["words"], voiced), [(0, 9), (18, 30)]
        )
        result = BENCHMARK.score_unwritten(gold, holed, voiced, True)
        self.assertEqual((result["exposed"], result["holes"]), (1, 1))

    def test_praat_binary_textgrid_is_read(self):
        def text(value):
            raw = value.encode("latin-1", errors="strict") if value.isascii() else None
            if raw is not None:
                return struct.pack(">H", len(raw)) + raw
            units = value.encode("utf-16-be")
            return struct.pack(">HH", 0xFFFF, len(units) // 2) + units

        def short(value):
            return bytes([len(value)]) + value.encode("ascii")

        intervals = [(0.0, 0.5, "_"), (0.5, 0.75, "c'"), (0.75, 1.0, "était")]
        data = b"ooBinaryFile" + short("TextGrid") + struct.pack(">dd", 0, 1)
        data += b"\x01" + struct.pack(">i", 1)
        data += short("IntervalTier") + text("words") + struct.pack(">dd", 0, 1)
        data += struct.pack(">i", len(intervals))
        for start, end, label in intervals:
            data += struct.pack(">dd", start, end) + text(label)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.TextGrid"
            path.write_bytes(data)
            self.assertEqual(BENCHMARK.praat_binary_tiers(path), {"words": intervals})
            path.write_bytes(data + b"\x00")
            with self.assertRaises(BENCHMARK.BenchmarkError):
                BENCHMARK.praat_binary_tiers(path)

    def test_cprom_gold_joins_clitics_and_skips_several_words(self):
        words = CPROM.gold_words(
            [(0.0, 0.2, "_"), (0.2, 0.3, "c'"), (0.3, 0.5, "est "),
             (0.5, 0.8, "il|y"), (0.8, 1.0, "%"), (1.0, 1.2, "euh")]
        )
        self.assertEqual(
            [(w.get("text"), w.get("scored")) for w in words],
            [(None, None), ("c'est", True), ("il y", False), (None, None),
             ("euh", True)],
        )
        self.assertEqual((words[1]["start"], words[1]["end"]), (0.2, 0.5))

    def test_cprom_chunks_cut_in_long_pauses_past_half_a_chunk(self):
        items = [
            {"text": "a", "start": 0.0, "end": 10.0, "scored": True},
            {"pause": True, "start": 10.0, "end": 11.0},
            {"text": "b", "start": 11.0, "end": 35.0, "scored": True},
            # Too short to cut in, though past half a chunk.
            {"pause": True, "start": 35.0, "end": 35.2},
            {"text": "d", "start": 35.2, "end": 40.0, "scored": True},
            {"pause": True, "start": 40.0, "end": 41.0},
            {"text": "c", "start": 41.0, "end": 50.0, "scored": True},
        ]
        chunks = CPROM.chunks_of(items, 50.0)
        self.assertEqual(
            [(start, end, [i["text"] for i in chunk]) for start, end, chunk in chunks],
            [(0.0, 40.5, ["a", "b", "d"]), (40.5, 50.0, ["c"])],
        )

    def test_cprom_transcript_counts_hesitations_repeats_and_cuts(self):
        reference = CPROM.reference_tokens(
            [{"text": "le"}, {"text": "le"}, {"text": "euh"},
             {"text": "incon-"}, {"text": "projet"}]
        )
        self.assertEqual(
            [(t["token"], t["disfluency"], t["repeat"], t["truncated"])
             for t in reference],
            [("le", False, False, False), ("le", False, True, False),
             ("euh", True, False, False), ("incon", False, False, True),
             ("projet", False, False, False)],
        )
        counts = CPROM.transcript_counts(
            reference, CPROM.transcript_tokens("Le, heu inconnu projet.")
        )
        self.assertEqual(
            {key: counts[key] for key in (
                "deletion", "substitution", "euh_written", "repeats_kept",
                "truncated_kept", "content_errors")},
            {"deletion": 1, "substitution": 1, "euh_written": 1,
             "repeats_kept": 0, "truncated_kept": 1, "content_errors": 2},
        )

    def test_cprom_phones_heard_inside_a_hand_placed_euh(self):
        words = [{"text": "il", "start": 0.0, "end": 0.2},
                 {"text": "euh", "start": 0.3, "end": 0.6},
                 {"text": "dit", "start": 0.8, "end": 1.0}]
        phones = [["i", 0.0, 0.02], ["ø", 0.4, 0.42], ["ə", 0.7, 0.72],
                  ["d", 0.8, 0.82]]
        self.assertEqual(
            CPROM.phone_counts(words, phones),
            {"euh": 1, "euh_any_phone": 1, "euh_hesitation_phone": 1,
             "phones_outside_words": 1, "hesitation_phones_outside_words": 1},
        )

    def test_cprom_filled_pauses_count_found_targets_and_false_alarms(self):
        targets = {"euh": [[1.0, 1.3]], "lengthening": [[3.0, 3.4]]}
        counts = CPROM.filled_pause_counts(
            targets, [[1.05, 1.35], [5.0, 5.3], [3.35, 3.5]]
        )
        self.assertEqual(
            counts,
            {"euh": 1, "euh_found": 1, "lengthening": 1, "lengthening_found": 0,
             "detections": 3, "false_alarms": 1},
        )

    def test_cprom_dropped_euh_found_in_a_gap_or_a_stretched_phone(self):
        chunk = {"words": [{"text": "porté", "start": 0.0, "end": 0.4},
                           {"text": "euh", "start": 0.5, "end": 0.9},
                           {"text": "sur", "start": 1.0, "end": 1.3},
                           {"text": "euh", "start": 1.4, "end": 1.8},
                           {"text": "la", "start": 1.9, "end": 2.0}]}
        targets = {"euh": [[0.5, 0.9], [1.4, 1.8]], "lengthening": []}
        tiers = {
            "words": {"entries": [[0.0, 0.4, "porté"], [1.0, 1.85, "sur"],
                                  [1.9, 2.0, "la"]]},
            "phones": {"entries": [[0.0, 0.2, "p"], [0.2, 0.4, "o"],
                                   [1.0, 1.1, "s"], [1.1, 1.85, "y"],
                                   [1.9, 2.0, "l"]]},
        }
        detections = [[0.55, 0.85], [1.45, 1.75], [0.05, 0.35]]
        counts = CPROM.dropped_hesitation_counts(
            chunk, targets, tiers, detections, [True] * 50
        )
        self.assertEqual(
            {k: counts[k] for k in ("euh", "dropped", "detector_found",
                                    "detector_alerts", "detector_unexplained")},
            {"euh": 2, "dropped": 2, "detector_found": 2, "detector_alerts": 2,
             "detector_unexplained": 0},
        )


if __name__ == "__main__":
    unittest.main()
