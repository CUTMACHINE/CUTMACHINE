from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unicodedata
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from sidecar.binary import ApplyResult, CutmachineBinary
from sidecar.eval import A1, CASES, operations_equal
from sidecar.planner import (
    AnthropicPlanner,
    OllamaPlanner,
    Plan,
    Planner,
    PlannerError,
)
from sidecar.repl import run_turn
from sidecar.resolve_bridge import (
    FLATTEN_SCHEMA,
    SCHEMA,
    FRAMING_PROPERTIES,
    TIMELINE_READ_SCHEMA,
    TIMELINE_SCHEMA,
    UNPUBLISHED_BY_API,
    ResolveBridgeError,
    collect,
    content_named_srt,
    count_srt_cues,
    count_subtitle_items,
    flatten_timeline,
    speed_facts,
    index_media_pool,
    integral_rate,
    is_transition_item,
    main as resolve_bridge_main,
    normalized,
    plan_render,
    read_named_timeline,
    read_timeline,
    read_xml_timeline,
    remap_to_sources,
    run_renders,
    send_timeline,
    send_timeline_xml,
    media_pool_folders,
    stamp_xml_timecodes,
    verify_imported_timeline,
)
from sidecar.schema import PLANNER_RESPONSE_SCHEMA


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name("eval-document.json")
BINARY = ROOT / "build" / "cutmachine"


def create_project_package(parent: Path, name: str = "Fixture") -> Path:
    document_text = FIXTURE.read_text(encoding="utf-8")
    document = json.loads(document_text)
    timeline_id = document["sequence"]["id"]
    package = parent / f"{name}.cutmachine-project"
    timelines = package / "Timelines"
    timelines.mkdir(parents=True)
    (timelines / f"{timeline_id}.json").write_text(
        document_text, encoding="utf-8")
    project = {
        "project_format": "cutmachine-project",
        "project_version": 2,
        "id": "01K60000000000000000000001",
        "name": name,
        "active_timeline_id": timeline_id,
        "timeline_snapshots": [document_text],
        "bin_metadata": [],
        "timeline_bin_ids": [],
    }
    project_path = package / "project.cutmachine.json"
    project_path.write_text(json.dumps(project), encoding="utf-8")
    manifest = {
        "format": "cutmachine-collection",
        "version": 2,
        "project": "project.cutmachine.json",
        "timelines": [{
            "id": timeline_id,
            "path": f"Timelines/{timeline_id}.json",
        }],
        "media": [],
    }
    (package / "manifest.json").write_text(
        json.dumps(manifest, separators=(",", ":")), encoding="utf-8")
    return project_path


class FakeResponse:
    def __init__(self, payload: dict[str, Any]) -> None:
        self.payload = payload

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: Any) -> None:
        return None

    def read(self) -> bytes:
        return json.dumps(self.payload).encode()


class RecordingOpener:
    def __init__(self, response: dict[str, Any]) -> None:
        self.response = response
        self.request: Any = None
        self.timeout: float | None = None

    def __call__(self, request: Any, timeout: float) -> FakeResponse:
        self.request = request
        self.timeout = timeout
        return FakeResponse(self.response)

    @property
    def body(self) -> dict[str, Any]:
        return json.loads(self.request.data)


def remove_plan() -> dict[str, Any]:
    return {
        "operation": {
            "type": "RemoveClip", "clip_id": A1, "exact_timeline": [],
        },
        "refusal": None,
    }


def trim_plan(
    edge: str, action: str, amount: int, unit: str = "Frames"
) -> dict[str, Any]:
    return {
        "operation": {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": edge,
            "trim_action": action,
            "trim_amount": {"value": amount, "unit": unit},
            "exact_clip": None,
        },
        "refusal": None,
    }


def add_marker_plan() -> dict[str, Any]:
    return {
        "operation": {
            "type": "AddMarker",
            "marker_id": "",
            "marker_name": "Sélection interview",
            "marker_time": {"value": 1001, "rate": 30000},
            "marker_color": "#33AAFF",
            "marker_category": "Montage",
        },
        "refusal": None,
    }


class PlannerProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not BINARY.exists():
            raise unittest.SkipTest("build/cutmachine is not available")
        cls.fixture_directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture_directory.cleanup)
        project = create_project_package(
            Path(cls.fixture_directory.name), "Planner")
        cls.timeline = CutmachineBinary(BINARY).describe(project)

    def test_ollama_receives_shared_schema_in_format(self) -> None:
        opener = RecordingOpener({
            "message": {"content": json.dumps(remove_plan())},
        })
        planner = OllamaPlanner(model="test-model", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        self.assertEqual(opener.body["format"], PLANNER_RESPONSE_SCHEMA)
        self.assertFalse(opener.body["stream"])
        self.assertEqual(opener.body["options"]["temperature"], 0)
        self.assertTrue(opener.request.full_url.endswith("/api/chat"))

    def test_describe_exposes_sequence_format_to_the_planner(self) -> None:
        sequence = self.timeline["sequence"]
        self.assertGreater(sequence["width"], 0)
        self.assertGreater(sequence["height"], 0)
        self.assertRegex(sequence["frame_rate"], r"^\d+/\d+$")

    def test_shared_schema_uses_no_unsupported_composition_keywords(self) -> None:
        encoded = json.dumps(PLANNER_RESPONSE_SCHEMA)
        self.assertNotIn('"oneOf"', encoded)
        self.assertNotIn('"anyOf"', encoded)
        self.assertNotIn('"allOf"', encoded)

    def test_anthropic_receives_same_schema_as_tool_input(self) -> None:
        opener = RecordingOpener({
            "content": [{
                "type": "tool_use",
                "name": AnthropicPlanner.TOOL_NAME,
                "input": remove_plan(),
            }],
        })
        planner = AnthropicPlanner(
            model="test-model", api_key="test-key", opener=opener)
        plan = planner.plan(self.timeline, "Supprime A1")
        self.assertEqual(plan.operation, remove_plan()["operation"])
        tool = opener.body["tools"][0]
        self.assertEqual(tool["input_schema"], PLANNER_RESPONSE_SCHEMA)
        self.assertTrue(tool["strict"])
        self.assertEqual(
            opener.body["tool_choice"],
            {"type": "tool", "name": AnthropicPlanner.TOOL_NAME})
        self.assertTrue(opener.request.full_url.endswith("/v1/messages"))

    def test_refusal_is_supported_by_both_response_shapes(self) -> None:
        refusal = {
            "operation": None,
            "refusal": {"reason": "Deux opérations seraient nécessaires."},
        }
        ollama = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(refusal)}}))
        anthropic = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": refusal}]}))
        self.assertEqual(
            ollama.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])
        self.assertEqual(
            anthropic.plan(self.timeline, "demande composée").refusal,
            refusal["refusal"]["reason"])

    def test_unknown_model_ulid_is_rejected_before_binary(self) -> None:
        invalid = remove_plan()
        invalid["operation"]["clip_id"] = "01K49999999999999999999999"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(invalid)}}))
        with self.assertRaisesRegex(PlannerError, "absent from the view"):
            planner.plan(self.timeline, "Supprime un clip inexistant")

    def test_older_ollama_missing_nullable_field_is_tolerated(self) -> None:
        response = remove_plan()["operation"]
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({"operation": response})}}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation, response)

    def test_backend_union_fields_are_normalized_to_canonical_operation(self) -> None:
        operation = {
            "type": "RemoveClip",
            "clip_id": A1,
            "edge": "Head",
            "trim_action": "Extend",
            "trim_amount": {"value": 99, "unit": "Frames"},
            "track_id": "irrelevant",
        }
        planner = AnthropicPlanner(api_key="x", opener=RecordingOpener({
            "content": [{"type": "tool_use", "name": AnthropicPlanner.TOOL_NAME,
                         "input": {"operation": operation,
                                   "refusal": {"reason": "irrelevant"}}}]}))
        self.assertEqual(
            planner.plan(self.timeline, "Supprime A1").operation,
            remove_plan()["operation"],
        )

    def test_trim_intent_is_converted_to_internal_delta_sign(self) -> None:
        expectations = (
            ("Head", "Shorten", 10),
            ("Head", "Extend", -10),
            ("Tail", "Shorten", -10),
            ("Tail", "Extend", 10),
        )
        for edge, action, expected_delta in expectations:
            with self.subTest(edge=edge, action=action):
                planner = OllamaPlanner(opener=RecordingOpener({
                    "message": {"content": json.dumps(
                        trim_plan(edge, action, 10))},
                }))
                operation = planner.plan(self.timeline, "trim").operation
                self.assertIsNotNone(operation)
                self.assertEqual(
                    operation["delta"],
                    {"value": expected_delta, "rate": 25},
                )

    def test_marker_plan_is_normalized_to_canonical_operation(self) -> None:
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(add_marker_plan())},
        }))
        operation = planner.plan(
            self.timeline, "Ajoute un marqueur pour la sélection interview"
        ).operation
        self.assertEqual(operation, {
            "type": "AddMarker",
            "marker": {
                "id": "",
                "name": "Sélection interview",
                "time": {"value": 1001, "rate": 30000},
                "color": "#33AAFF",
                "category": "Montage",
            },
            "insertion_index": -1,
        })

    def test_marker_alias_overrides_a_model_guessed_id(self) -> None:
        timeline = json.loads(json.dumps(self.timeline))
        marker_id = "01K81111111111111111111111"
        timeline["markers"] = [{
            "alias": "K1", "id": marker_id, "name": "Choix",
            "time": {"frames": 10, "seconds": 0.4},
            "color": "#33AAFF", "category": "Montage",
        }]
        response = {
            "operation": {
                "type": "RemoveMarker",
                "marker_id": "01K89999999999999999999999",
            },
            "refusal": None,
        }
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(response)},
        }))
        self.assertEqual(
            planner.plan(timeline, "Supprime K1").operation,
            {"type": "RemoveMarker", "marker_id": marker_id},
        )

    def test_explicit_trim_language_controls_sign_timebase_and_type(self) -> None:
        response = trim_plan("Head", "Shorten", 99)
        response["operation"]["type"] = "InsertClip"
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(response)},
        }))
        operation = planner.plan(
            self.timeline, "Récupère 2 secondes avant le début actuel de A1."
        ).operation
        self.assertEqual(operation, {
            "type": "TrimClip",
            "clip_id": A1,
            "edge": "Head",
            "delta": {"value": -50, "rate": 25},
            "exact_clip": None,
        })

        shorten = planner.plan(
            self.timeline, "Coupe les 10 premières images de A1."
        ).operation
        self.assertEqual(shorten["edge"], "Head")
        self.assertEqual(shorten["delta"], {"value": 10, "rate": 25})

    def test_explicit_timeline_frame_is_absolute(self) -> None:
        operation = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][0]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 50, "rate": 25},
            "duration": {"value": 10, "rate": 25},
            "timeline_in": {"value": 75, "rate": 25},
        }
        planner = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": operation, "refusal": None})},
        }))
        normalized = planner.plan(
            self.timeline,
            "Dans le trou après A1, à l'image 50, place 10 images.",
        ).operation
        self.assertEqual(normalized["timeline_in"], {"value": 50, "rate": 25})

    def test_ordinal_entities_override_model_ulids(self) -> None:
        wrong_clip = remove_plan()
        wrong_clip["operation"]["clip_id"] = next(
            item["id"] for item in self.timeline["tracks"][1]["items"]
            if item["type"] == "clip")
        remove = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps(wrong_clip)},
        })).plan(
            self.timeline, "Enlève le deuxième clip de la piste vidéo 1."
        ).operation
        primary_clips = [
            item for item in self.timeline["tracks"][0]["items"]
            if item["type"] == "clip"]
        self.assertEqual(remove["clip_id"], primary_clips[1]["id"])

        insert = {
            "type": "InsertClip",
            "track_id": self.timeline["tracks"][1]["id"],
            "source_id": self.timeline["sources"][1]["id"],
            "source_in": {"value": 300, "rate": 25},
            "duration": {"value": 20, "rate": 25},
            "timeline_in": {"value": 1, "rate": 25},
        }
        normalized = OllamaPlanner(opener=RecordingOpener({
            "message": {"content": json.dumps({
                "operation": insert, "refusal": None})},
        })).plan(
            self.timeline,
            "Ajoute à la fin de la piste vidéo 1 vingt images de "
            "interview.mov depuis l'image source 300.",
        ).operation
        self.assertEqual(normalized["track_id"], self.timeline["tracks"][0]["id"])
        self.assertEqual(normalized["source_id"], self.timeline["sources"][0]["id"])
        self.assertEqual(normalized["timeline_in"], {"value": 150, "rate": 25})


class BinaryIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        self.directory = tempfile.TemporaryDirectory()
        self.document = create_project_package(
            Path(self.directory.name), "Integration")
        self.binary = CutmachineBinary(BINARY)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_describe_and_named_refusal(self) -> None:
        timeline = self.binary.describe(self.document)
        self.assertEqual(timeline["tracks"][0]["items"][0]["alias"], "A1")
        before = self.document.read_bytes()
        result = self.binary.apply_operation(self.document, {
            "type": "RemoveClip",
            "clip_id": "01K49999999999999999999999",
            "exact_timeline": [],
        })
        self.assertFalse(result.ok)
        self.assertEqual(result.error, "UnknownClip")
        self.assertEqual(self.document.read_bytes(), before)

    def test_reordered_operation_is_serialized_canonically(self) -> None:
        # JSON object order has no semantics, while the existing C++ operation
        # reader consumes its canonical serializer order.
        operation = {
            "exact_clip": None,
            "delta": {"rate": 25, "value": -1},
            "edge": "Tail",
            "clip_id": A1,
            "type": "TrimClip",
        }
        result = self.binary.apply_operation(self.document, operation)
        self.assertTrue(result.ok, f"{result.error}: {result.detail}")

    def test_markers_are_addressable_through_describe_and_apply_op(self) -> None:
        add = self.binary.apply_operation(self.document, {
            "type": "AddMarker",
            "marker": {
                "id": "",
                "name": "Choix interview",
                "time": {"value": 15, "rate": 25},
                "color": "#33AAFF",
                "category": "Montage",
            },
            "insertion_index": -1,
        })
        self.assertTrue(add.ok, f"{add.error}: {add.detail}")
        timeline = self.binary.describe(self.document)
        self.assertEqual(timeline["markers"][0]["alias"], "K1")
        marker_id = timeline["markers"][0]["id"]

        update = self.binary.apply_operation(self.document, {
            "type": "UpdateMarker",
            "marker_id": marker_id,
            "name": "Choix final",
            "time": {"value": 20, "rate": 25},
            "color": "#FFAA33",
            "category": "Validation",
        })
        self.assertTrue(update.ok, f"{update.error}: {update.detail}")
        self.assertEqual(
            self.binary.describe(self.document)["markers"][0]["name"],
            "Choix final",
        )

        remove = self.binary.apply_operation(self.document, {
            "type": "RemoveMarker", "marker_id": marker_id,
        })
        self.assertTrue(remove.ok, f"{remove.error}: {remove.detail}")
        self.assertEqual(self.binary.describe(self.document)["markers"], [])


class FakePlanner(Planner):
    def __init__(self) -> None:
        self.errors: list[dict[str, Any] | None] = []

    @property
    def backend_name(self) -> str:
        return "fake"

    def plan(self, timeline: dict[str, Any], instruction: str,
             previous_error: dict[str, Any] | None = None) -> Plan:
        self.errors.append(previous_error)
        return Plan(operation=remove_plan()["operation"])


class FakeBinary:
    def __init__(self) -> None:
        self.apply_count = 0

    def describe(self, document: Path) -> dict[str, Any]:
        return {"sources": [], "tracks": [], "duration": {"frames": 0,
                                                               "seconds": 0.0}}

    def apply_operation(self, document: Path,
                        operation: dict[str, Any]) -> ApplyResult:
        self.apply_count += 1
        if self.apply_count == 1:
            return ApplyResult(False, error="Overlap", detail="test overlap")
        return ApplyResult(True, doc_hash="abc123")


class ReplTests(unittest.TestCase):
    def test_one_retry_includes_named_binary_error(self) -> None:
        planner = FakePlanner()
        binary = FakeBinary()
        answers = iter(("o", "o"))
        output: list[str] = []
        run_turn(planner, binary, Path("unused.json"), "supprime A1",
                 input_fn=lambda _: next(answers), print_fn=output.append)
        self.assertEqual(binary.apply_count, 2)
        self.assertIsNone(planner.errors[0])
        self.assertEqual(planner.errors[1]["error"], "Overlap")
        self.assertIn("abc123", output[-1])


class EvaluationTests(unittest.TestCase):
    def test_corpus_has_exactly_fifteen_cases(self) -> None:
        self.assertEqual(len(CASES), 15)

    def test_semantic_comparison_accepts_equivalent_timebase(self) -> None:
        expected = CASES[4].expected
        actual = json.loads(json.dumps(expected))
        actual["delta"] = {"value": -20, "rate": 50}
        self.assertTrue(operations_equal(actual, expected))

    def test_all_expected_operations_are_accepted_on_fresh_documents(self) -> None:
        if not BINARY.exists():
            self.skipTest("build/cutmachine is not available")
        binary = CutmachineBinary(BINARY)
        with tempfile.TemporaryDirectory() as directory:
            for index, case in enumerate(CASES):
                document = create_project_package(
                    Path(directory), f"Case-{index}")
                result = binary.apply_operation(document, case.expected)
                self.assertTrue(
                    result.ok,
                    f"invalid oracle for case {index + 1}: "
                    f"{result.error}: {result.detail}",
                )


class FakeResolveClip:
    """Duck type of a Resolve MediaPoolItem, property bag included."""

    def __init__(self, **properties: Any) -> None:
        self.properties = properties

    def GetClipProperty(self, key: str | None = None):
        # The real API answers the whole property bag with no argument and a
        # single value with a key. collect() uses the first form, the send
        # path the second, so the fake has to do both.
        if key is None:
            return self.properties
        return self.properties.get(key, "")

    def GetUniqueId(self) -> str:
        return self.properties.get("uid", "")


class FakeResolveFolder:
    def __init__(self, name: str, clips=None, folders=None) -> None:
        self.name = name
        self.clips = clips or []
        self.folders = folders or []

    def GetName(self) -> str:
        return self.name

    def GetClipList(self):
        return self.clips

    def GetSubFolderList(self):
        return self.folders


class ResolveBridgeTests(unittest.TestCase):
    def media_pool(self) -> FakeResolveFolder:
        rush = FakeResolveClip(**{
            "File Path": "/rushes/C8015.MP4",
            "Clip Name": "C8015.MP4",
            "uid": "5d45",
        })
        loose = FakeResolveClip(**{
            "File Path": "/rushes/C8035.MP4",
            "File Name": "C8035.MP4",
        })
        timeline = FakeResolveClip(**{
            "File Path": "",
            "Clip Name": "DERUSH",
            "Type": "Timeline",
        })
        sub_bin = FakeResolveFolder("Jour 1", clips=[rush])
        rushes = FakeResolveFolder("1_RUSHES", folders=[sub_bin])
        tl = FakeResolveFolder("TL", clips=[timeline])
        return FakeResolveFolder("Master", clips=[loose],
                                 folders=[rushes, tl])

    def test_walks_the_tree_into_flat_lists(self) -> None:
        collected = collect(self.media_pool())
        names = {b["name"]: b for b in collected["bins"]}
        self.assertEqual(set(names), {"1_RUSHES", "Jour 1", "TL"})
        self.assertEqual(names["1_RUSHES"]["parent_key"], "")
        self.assertEqual(names["Jour 1"]["parent_key"],
                         names["1_RUSHES"]["key"])
        self.assertEqual(len(set(b["key"] for b in collected["bins"])), 3)

    def test_root_clips_carry_the_empty_bin_key(self) -> None:
        collected = collect(self.media_pool())
        loose = [c for c in collected["clips"] if c["name"] == "C8035.MP4"]
        self.assertEqual(len(loose), 1)
        self.assertEqual(loose[0]["bin_key"], "")
        self.assertNotIn("resolve_uid", loose[0])

    def test_nested_clip_keeps_its_bin_and_identity(self) -> None:
        collected = collect(self.media_pool())
        names = {b["name"]: b for b in collected["bins"]}
        nested = [c for c in collected["clips"] if c["name"] == "C8015.MP4"]
        self.assertEqual(len(nested), 1)
        self.assertEqual(nested[0]["bin_key"], names["Jour 1"]["key"])
        self.assertEqual(nested[0]["resolve_uid"], "5d45")
        self.assertEqual(nested[0]["path"], "/rushes/C8015.MP4")

    def test_entries_without_a_file_are_reported_not_dropped(self) -> None:
        collected = collect(self.media_pool())
        self.assertEqual([c["name"] for c in collected["clips"]].count(
            "DERUSH"), 0)
        skipped = collected["skipped"]
        self.assertEqual(len(skipped), 1)
        self.assertEqual(skipped[0]["name"], "DERUSH")
        self.assertEqual(skipped[0]["reason"], "Timeline")

    def test_sibling_bins_sharing_a_name_get_distinct_keys(self) -> None:
        pool = FakeResolveFolder("Master", folders=[
            FakeResolveFolder("Jour 01"), FakeResolveFolder("Jour 01")])
        collected = collect(pool)
        keys = [b["key"] for b in collected["bins"]]
        self.assertEqual(len(keys), 2)
        self.assertEqual(len(set(keys)), 2)

    def test_schema_matches_the_importer(self) -> None:
        header = (ROOT / "src" / "ResolveImport.h").read_text(
            encoding="utf-8")
        self.assertIn(f'"{SCHEMA}"', header)


class FakeResolveTimeline:
    """Resolve compte les images depuis le timecode de départ de la séquence.

    90000 = 01:00:00:00 à 25 i/s, ce que renvoie un vrai projet. Le pont doit
    décaler chaque `recordFrame` d'autant, sinon les plans repartent à zéro et
    se réempilent bout à bout -- exactement le défaut que le schéma v2 corrige.
    """

    def __init__(self, name: str, start_frame: int = 90000,
                 subtitle_tracks: int = 0) -> None:
        self.name = name
        self.start_frame = start_frame
        self.video_tracks = 1
        self.subtitle_tracks = subtitle_tracks
        # Items per subtitle track, 1-based like Resolve's track indices.
        self.subtitle_items: dict[int, list[str]] = {}

    def GetTrackCount(self, kind: str) -> int:
        if kind == "subtitle":
            return self.subtitle_tracks
        return self.video_tracks if kind == "video" else 1

    def AddTrack(self, kind: str) -> bool:
        if kind == "subtitle":
            self.subtitle_tracks += 1
            return True
        if kind != "video":
            return False
        self.video_tracks += 1
        return True

    def GetItemListInTrack(self, kind: str, index: int):
        if kind != "subtitle":
            return []
        return list(self.subtitle_items.get(index, []))

    def GetStartFrame(self) -> int:
        return self.start_frame


class FakeMediaPool:
    """Media Pool with Resolve's two subtitle quirks.

    ImportMedia hands back the item already imported from a path, whatever
    the file holds now; AppendToTimeline stacks a subtitle item's cues on S1
    whatever trackIndex says.
    """

    def __init__(self, root, timelines=None, subtitle_tracks: int = 0) -> None:
        self.root = root
        self.created = None
        self.timeline = None
        self.appended = None
        # Ordered log: subtitling depends on *when* AppendToTimeline runs,
        # not only on what it receives.
        self.appends = []
        self.imported = []
        self.by_path: dict[str, FakeResolveClip] = {}
        self.subtitle_tracks = subtitle_tracks
        # Cues Resolve merges or drops on import, to make placed != file.
        self.lost_cues = 0

    def GetRootFolder(self):
        return self.root

    def CreateEmptyTimeline(self, name):
        self.created = name
        self.timeline = FakeResolveTimeline(
            name, subtitle_tracks=self.subtitle_tracks)
        return self.timeline

    def AppendToTimeline(self, infos):
        self.appended = infos
        self.appends.append((self.timeline.video_tracks,
                             self.timeline.subtitle_tracks, infos))
        for info in infos:
            cues = info["mediaPoolItem"].GetClipProperty("cues")
            if cues:
                self.timeline.subtitle_items.setdefault(1, []).extend(
                    f"cue {index + 1}" for index in range(cues))
        return True

    def ImportMedia(self, paths):
        self.imported.extend(paths)
        path = paths[0]
        if path not in self.by_path:
            self.by_path[path] = FakeResolveClip(**{
                "File Path": path, "File Name": Path(path).name,
                "cues": count_srt_cues(path) - self.lost_cues})
        return [self.by_path[path]]


class FakeResolveProject:
    def __init__(self, pool) -> None:
        self.pool = pool

    def GetMediaPool(self):
        return self.pool

    def GetName(self):
        return "projet"


class FakeResolveApp:
    def __init__(self, project) -> None:
        self.project = project

    def GetProjectManager(self):
        return self

    def GetCurrentProject(self):
        return self.project

    def GetVersionString(self):
        return "20.3.1.6"


class ResolveSendTests(unittest.TestCase):
    ACCENTED = "/Volumes/LaCie/Cr\u00e9puscule/C0104.MP4"

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        # Where the bridge writes its content-named copies; never the real
        # ~/Library during a test.
        self.copies = str(self.scratch / "copies")

    def app(self, stored_path: str,
            subtitle_tracks: int = 0) -> tuple[FakeResolveApp, FakeMediaPool]:
        clip = FakeResolveClip(**{
            "File Path": stored_path, "File Name": "C0104.MP4"})
        pool = FakeMediaPool(FakeResolveFolder("Master", clips=[clip]),
                             subtitle_tracks=subtitle_tracks)
        return FakeResolveApp(FakeResolveProject(pool)), pool

    def timeline(self, path: str) -> dict[str, Any]:
        return {"schema": TIMELINE_SCHEMA, "name": "MONTAGE",
                "clips": [{"path": path, "filename": "C0104.MP4",
                           "start_frame": 0, "end_frame": 100,
                           "video_layer": 0, "record_frame": 0,
                           "with_audio": True}]}

    def overlay(self, path: str) -> dict[str, Any]:
        """Un plan de coupe posé par-dessus le plan parlant."""
        timeline = self.timeline(path)
        timeline["clips"].append({
            "path": path, "filename": "C0104.MP4",
            "start_frame": 300, "end_frame": 350,
            "video_layer": 1, "record_frame": 40, "with_audio": False})
        return timeline

    def test_rebuilds_the_cut_in_resolve(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.timeline(self.ACCENTED))
        self.assertEqual(pool.created, "MONTAGE")
        self.assertEqual(len(pool.appended), 1)
        self.assertEqual(pool.appended[0]["startFrame"], 0)
        self.assertEqual(pool.appended[0]["endFrame"], 100)
        self.assertIn("1 plan", message)

    def test_positions_each_plan_from_the_timeline_start(self) -> None:
        # Sans le décalage, recordFrame vaudrait 0 et Resolve replacerait le
        # plan au tout début au lieu de l'endroit que le montage lui donne.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.overlay(self.ACCENTED))
        self.assertEqual(pool.appended[0]["recordFrame"], 90000)
        self.assertEqual(pool.appended[1]["recordFrame"], 90040)

    def test_creates_a_track_per_layer_and_addresses_it(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.overlay(self.ACCENTED))
        self.assertEqual(pool.timeline.video_tracks, 2)
        self.assertEqual(pool.appended[0]["trackIndex"], 1)
        self.assertEqual(pool.appended[1]["trackIndex"], 2)
        self.assertIn("2 pistes", message)

    def test_an_overlay_travels_without_its_sound(self) -> None:
        # Resolve rapporte le son de chaque plan : laisser passer celui d'un
        # plan de coupe poserait son ambiance sous l'interview recouverte.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.overlay(self.ACCENTED))
        self.assertNotIn("mediaType", pool.appended[0])
        self.assertEqual(pool.appended[1]["mediaType"], 1)

    def test_an_explicit_name_overrides_the_montage_name(self) -> None:
        # Resolve refuse un nom déjà pris : sans surcharge, une révision du
        # même montage ne pourrait jamais être renvoyée.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(self.ACCENTED), "MONTAGE v3")
        self.assertEqual(pool.created, "MONTAGE v3")

    def test_without_an_override_the_montage_name_is_kept(self) -> None:
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(self.ACCENTED), None)
        self.assertEqual(pool.created, "MONTAGE")

    def srt(self, cues: int = 3, name: str = "cues.srt") -> str:
        path = self.scratch / name
        with open(path, "w", encoding="utf-8") as handle:
            for index in range(cues):
                handle.write(
                    f"{index + 1}\n"
                    f"00:00:0{index},000 --> 00:00:0{index + 1},000\n"
                    f"carton {index + 1}\n\n")
        return str(path)

    def test_subtitles_land_before_any_clip(self) -> None:
        # AppendToTimeline pose un élément de sous-titres après ce que la
        # timeline contient déjà : sur une timeline remplie, les cartons
        # tombent derrière le dernier plan et la doublent en longueur. Le seul
        # ordre qui marche est piste de sous-titres et cues d'abord, plans
        # ensuite -- c'est cet ordre que ce test tient.
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.overlay(self.ACCENTED),
                                srt_path=self.srt(),
                                srt_directory=self.copies)
        self.assertEqual(len(pool.appends), 2)
        subtitles, clips = pool.appends
        # Au premier appel la timeline n'a encore qu'une piste vidéo : les
        # couches de recouvrement sont ajoutées après, donc rien ne précède.
        self.assertEqual(subtitles[0], 1)
        self.assertEqual(subtitles[1], 1)
        self.assertEqual(len(subtitles[2]), 1)
        self.assertEqual(len(clips[2]), 2)
        self.assertIn("3 sous-titre(s)", message)

    def test_a_subtitle_clipinfo_carries_nothing_else(self) -> None:
        # Y ajouter startFrame, endFrame, recordFrame ou mediaType fait
        # planter le pont, et recordFrame est de toute façon ignoré.
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(self.ACCENTED), srt_path=self.srt(),
                      srt_directory=self.copies)
        info = pool.appends[0][2][0]
        self.assertEqual(sorted(info), ["mediaPoolItem", "trackIndex"])
        self.assertEqual(info["trackIndex"], 1)

    def test_a_cut_without_subtitles_appends_once(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.timeline(self.ACCENTED))
        self.assertEqual(len(pool.appends), 1)
        self.assertEqual(pool.timeline.subtitle_tracks, 0)
        self.assertNotIn("sous-titre", message)

    def test_a_cue_count_that_can_be_compared_to_resolve(self) -> None:
        # C'est le seul contrôle qui attrape le cache de Resolve, qui rend
        # l'ancien .srt quand on réécrit le même chemin.
        self.assertEqual(count_srt_cues(self.srt(7)), 7)

    def test_placed_subtitles_equal_to_the_cues_pass(self) -> None:
        app, pool = self.app(self.ACCENTED)
        message = send_timeline(app, self.timeline(self.ACCENTED),
                                srt_path=self.srt(4),
                                srt_directory=self.copies)
        self.assertEqual(count_subtitle_items(pool.timeline), 4)
        self.assertIn("4 sous-titre(s)", message)

    def test_a_placed_count_that_differs_fails_before_any_clip(self) -> None:
        # Resolve a posé autre chose que le fichier : l'échec nomme les deux
        # nombres, et aucun plan n'est ajouté à la timeline incomplète.
        app, pool = self.app(self.ACCENTED)
        pool.lost_cues = 1
        with self.assertRaises(ResolveBridgeError) as raised:
            send_timeline(app, self.timeline(self.ACCENTED),
                          srt_path=self.srt(3), srt_directory=self.copies)
        message = str(raised.exception)
        self.assertIn("2 sous-titre(s)", message)
        self.assertIn("3 cue(s)", message)
        self.assertIn("MONTAGE", message)
        self.assertEqual(len(pool.appends), 1)

    def test_a_stale_media_pool_item_is_caught(self) -> None:
        # Le piège réel : un élément du Media Pool importé plus tôt depuis le
        # même chemin, avec l'ancien sous-titrage. Le pont l'importe ici tel
        # quel pour prouver que le comptage l'attrape.
        app, pool = self.app(self.ACCENTED)
        srt = self.srt(5)
        copy = content_named_srt(srt, self.copies)
        pool.by_path[copy] = FakeResolveClip(**{
            "File Path": copy, "File Name": Path(copy).name, "cues": 8})
        with self.assertRaises(ResolveBridgeError) as raised:
            send_timeline(app, self.timeline(self.ACCENTED), srt_path=srt,
                          srt_directory=self.copies)
        self.assertIn("8 sous-titre(s)", str(raised.exception))
        self.assertIn("5 cue(s)", str(raised.exception))

    def test_a_rewritten_srt_is_imported_afresh(self) -> None:
        # Réécrire le même .srt puis renvoyer : Resolve rendrait l'élément de
        # la version précédente si le pont importait ce chemin. La copie
        # porte l'empreinte du contenu, donc un nom neuf.
        app, pool = self.app(self.ACCENTED)
        srt = self.srt(3)
        send_timeline(app, self.timeline(self.ACCENTED), "v1", srt_path=srt,
                      srt_directory=self.copies)
        self.srt(7)
        message = send_timeline(app, self.timeline(self.ACCENTED), "v2",
                                srt_path=srt, srt_directory=self.copies)
        self.assertEqual(len(set(pool.imported)), 2)
        self.assertNotIn(srt, pool.imported)
        self.assertEqual(count_subtitle_items(pool.timeline), 7)
        self.assertIn("7 sous-titre(s)", message)

    def test_the_same_content_keeps_the_same_copy(self) -> None:
        first = content_named_srt(self.srt(3), self.copies)
        second = content_named_srt(self.srt(3, "autre.srt"), self.copies)
        again = content_named_srt(self.srt(3), self.copies)
        self.assertEqual(first, again)
        self.assertNotEqual(first, second)
        self.assertTrue(Path(first).name.startswith("cues-"))
        self.assertEqual(Path(first).read_bytes(),
                         Path(self.srt(3)).read_bytes())

    def test_every_subtitle_track_is_counted(self) -> None:
        # La timeline a déjà une piste de sous-titres : AddTrack crée S2, mais
        # AppendToTimeline empile tout sur S1. Compter la seule piste ajoutée
        # donnerait zéro.
        app, pool = self.app(self.ACCENTED, subtitle_tracks=1)
        message = send_timeline(app, self.timeline(self.ACCENTED),
                                srt_path=self.srt(3),
                                srt_directory=self.copies)
        self.assertEqual(pool.timeline.subtitle_tracks, 2)
        self.assertEqual(pool.timeline.GetItemListInTrack("subtitle", 2), [])
        self.assertIn("3 sous-titre(s)", message)

    def test_the_command_exits_non_zero_on_a_mismatch(self) -> None:
        app, pool = self.app(self.ACCENTED)
        pool.lost_cues = 2
        timeline = self.scratch / "timeline.json"
        timeline.write_text(json.dumps(self.timeline(self.ACCENTED)),
                            encoding="utf-8")
        errors = io.StringIO()
        with mock.patch("sidecar.resolve_bridge.connect", return_value=app), \
                mock.patch("sidecar.resolve_bridge.SRT_IMPORT_DIRECTORY",
                           self.copies), \
                contextlib.redirect_stderr(errors):
            status = resolve_bridge_main(
                ["--send", str(timeline), "--srt", self.srt(3)])
        self.assertEqual(status, 1)
        self.assertIn("1 sous-titre(s)", errors.getvalue())
        self.assertIn("3 cue(s)", errors.getvalue())

    def test_matches_across_unicode_normalizations(self) -> None:
        # CUTMACHINE resolves paths through the filesystem and gets NFD;
        # Resolve reports what it stored. Raw string comparison would fail on
        # every accented folder name.
        decomposed = unicodedata.normalize("NFD", self.ACCENTED)
        self.assertNotEqual(decomposed, self.ACCENTED)
        app, pool = self.app(self.ACCENTED)
        send_timeline(app, self.timeline(decomposed))
        self.assertEqual(len(pool.appended), 1)

    def test_falls_back_to_the_filename(self) -> None:
        app, pool = self.app("/ailleurs/C0104.MP4")
        send_timeline(app, self.timeline("/Volumes/autre/C0104.MP4"))
        self.assertEqual(len(pool.appended), 1)

    def test_refuses_a_rush_absent_from_the_media_pool(self) -> None:
        app, pool = self.app(self.ACCENTED)
        timeline = self.timeline(self.ACCENTED)
        timeline["clips"][0]["filename"] = "INCONNU.MP4"
        timeline["clips"][0]["path"] = "/ailleurs/INCONNU.MP4"
        with self.assertRaises(ResolveBridgeError) as raised:
            send_timeline(app, timeline)
        self.assertIn("INCONNU.MP4", str(raised.exception))
        self.assertIsNone(pool.appended)

    def test_refuses_an_unexpected_schema(self) -> None:
        app, _ = self.app(self.ACCENTED)
        timeline = self.timeline(self.ACCENTED)
        timeline["schema"] = "autre.v1"
        with self.assertRaises(ResolveBridgeError):
            send_timeline(app, timeline)

    def test_indexes_every_nested_folder(self) -> None:
        clip = FakeResolveClip(**{"File Path": "/a/B.MP4",
                                  "File Name": "B.MP4"})
        nested = FakeResolveFolder("Master", folders=[
            FakeResolveFolder("1_RUSHES", clips=[clip])])
        index = index_media_pool(nested)
        self.assertIn(normalized("/a/B.MP4"), index["by_path"])

    def test_timeline_schema_matches_the_exporter(self) -> None:
        header = (ROOT / "src" / "ResolveExport.cc").read_text(
            encoding="utf-8")
        self.assertIn(TIMELINE_SCHEMA, header)


class FakeTimelineItem:
    """Duck type of a Resolve TimelineItem, quirks included."""

    def __init__(self, name: str, start: int, duration: int, path: str,
                 source_start: int = 0, source_end_error: int = 1,
                 record_span: int | None = None,
                 left_offset: int = 0, right_offset: int | None = None,
                 media_frames: int | None = None,
                 media_fps: str = "50.0") -> None:
        self.name = name
        self.start = start
        self.duration = duration
        self.record_span = duration if record_span is None else record_span
        self.path = path
        self.source_start = source_start
        # Measured shape (Resolve 20.3.1): the offsets are in timeline frames
        # and already carry the speed, so left + span + right is the whole
        # media as this clip plays it. The defaults describe a clip at speed 1
        # in a 25 i/s timeline over a 50 i/s media, which is what every test
        # that does not care about speed wants.
        self.left_offset = left_offset
        span = self.record_span
        self.media_fps = media_fps
        if media_frames is None:
            media_frames = (left_offset + span
                            + (0 if right_offset is None else right_offset)) * 2
        self.media_frames = media_frames
        if right_offset is None:
            right_offset = media_frames // 2 - left_offset - span
        self.right_offset = right_offset
        # Measured on the real project: GetSourceEndFrame is short of the
        # duration by one frame on some clips and two on others.
        self.source_end_error = source_end_error

    def GetName(self) -> str:
        return self.name

    def GetStart(self) -> int:
        return self.start

    def GetEnd(self) -> int:
        return self.start + self.record_span

    def GetDuration(self) -> int:
        return self.duration

    def GetSourceStartFrame(self) -> int:
        return self.source_start

    def GetSourceEndFrame(self) -> int:
        return self.source_start + self.duration - self.source_end_error

    def GetLeftOffset(self) -> int:
        return self.left_offset

    def GetRightOffset(self) -> int:
        return self.right_offset

    def GetMediaPoolItem(self):
        if not self.path:
            return None
        return FakeResolveClip(**{"File Path": self.path,
                                  "File Name": self.path.split("/")[-1],
                                  "Frames": str(self.media_frames),
                                  "FPS": self.media_fps})


class FakeSourceTimeline:
    def __init__(self, name: str, tracks, start_frame: int = 90000,
                 rate: str = "25.0") -> None:
        self.name = name
        self.tracks = tracks          # {track index: [items]}
        self.start_frame = start_frame
        self.rate = rate

    def GetName(self) -> str:
        return self.name

    def GetStartFrame(self) -> int:
        return self.start_frame

    def GetEndFrame(self) -> int:
        last = max((i.GetEnd() for items in self.tracks.values()
                    for i in items), default=self.start_frame)
        return last

    def GetTrackCount(self, kind: str) -> int:
        return max(self.tracks) if kind == "video" else 1

    def GetItemListInTrack(self, kind: str, index: int):
        return self.tracks.get(index, []) if kind == "video" else []

    def GetSetting(self, key: str) -> str:
        return self.rate if key == "timelineFrameRate" else ""


class AudioSpeedFactsTests(unittest.TestCase):
    """Un média sans images : c'est la moitié de tout montage réel.

    Mesuré le 22 septembre 2026 : Resolve laisse `Frames` vide sur un média
    audio, ce qui faisait refuser toute timeline portant un son, quelle que
    soit son image. Sa `Duration` en timecode, lue à la cadence rendue, donne
    le même compte — vérifié sur quatre lits de musique et d'effets.
    """

    class FakeAudioMedia:
        def __init__(self, duration: str = "00:01:13:22",
                     fps: str = "25.0", frames: str = "") -> None:
            self.props = {"Duration": duration, "FPS": fps, "Frames": frames}

        def GetClipProperty(self, key):
            return self.props.get(key, "")

    class FakeAudioItem:
        def __init__(self, left: int = 0, right: int = 0) -> None:
            self.left, self.right = left, right

        def GetLeftOffset(self) -> int:
            return self.left

        def GetRightOffset(self) -> int:
            return self.right

    def test_an_audio_media_falls_back_to_its_duration(self) -> None:
        facts = speed_facts(self.FakeAudioItem(), self.FakeAudioMedia())
        self.assertNotIn("speed_unmeasurable", facts)
        # 00:01:13:22 à 25 i/s = (60+13)*25 + 22, la mesure relevée.
        self.assertEqual(facts["media_frames"], 1847)
        self.assertEqual(facts["media_rate"], 25)

    def test_a_video_media_still_uses_its_frame_count(self) -> None:
        media = self.FakeAudioMedia(duration="00:00:10:28", fps="50.0",
                                    frames="528")
        facts = speed_facts(self.FakeAudioItem(), media)
        self.assertEqual(facts["media_frames"], 528)

    def test_neither_frames_nor_duration_is_refused_not_assumed(self) -> None:
        media = self.FakeAudioMedia(duration="", frames="")
        facts = speed_facts(self.FakeAudioItem(), media)
        self.assertIn("speed_unmeasurable", facts)

    def test_a_drop_frame_timecode_is_refused(self) -> None:
        media = self.FakeAudioMedia(duration="00:01:13;22", frames="")
        facts = speed_facts(self.FakeAudioItem(), media)
        self.assertIn("speed_unmeasurable", facts)


class FlattenTests(unittest.TestCase):
    PATH_A = "/rushes/A044_C125.braw"
    PATH_B = "/rushes/A044_C126.braw"

    def timeline(self, **kwargs) -> FakeSourceTimeline:
        items = [
            FakeTimelineItem("C125", 90000, 216, self.PATH_A,
                             source_end_error=1),
            # The second clip is the one whose GetSourceEndFrame is off by two.
            FakeTimelineItem("C126", 90216, 138, self.PATH_B,
                             source_end_error=2),
        ]
        return FakeSourceTimeline("ITW_Exemple", {1: items}, **kwargs)

    def test_segments_are_relative_to_the_timeline_start(self) -> None:
        flat = flatten_timeline(self.timeline())
        self.assertEqual(flat["frame_rate"], 25)
        self.assertEqual(flat["duration"], 354)
        self.assertEqual(
            [(s["record_start"], s["record_end"]) for s in flat["segments"]],
            [(0, 216), (216, 354)])

    def test_source_length_comes_from_the_duration_not_the_end_frame(self):
        # GetSourceEndFrame disagrees with GetDuration by one frame on C125
        # and two on C126. Trusting it would shorten the segments; the record
        # span must stay the full duration for both.
        flat = flatten_timeline(self.timeline())
        spans = [s["record_end"] - s["record_start"] for s in flat["segments"]]
        self.assertEqual(spans, [216, 138])

    def test_a_gap_is_refused(self) -> None:
        items = [FakeTimelineItem("C125", 90000, 216, self.PATH_A),
                 FakeTimelineItem("C126", 90300, 138, self.PATH_B)]
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertIn("trou", str(caught.exception))

    def test_a_retime_is_refused(self) -> None:
        """The shape Resolve really produces for a clip slowed to 50%.

        Measured 22 September 2026 on Resolve 20.3.1: a 1776-frame rush at
        50 i/s in a 25 i/s timeline, 83 frames long on the timeline. At 100%
        the offsets total 888; at 50% they total 1776, because each source
        frame now occupies twice as many timeline frames. Note that
        GetDuration and GetEnd-GetStart are 83 in *both* cases -- the old
        guard compared exactly those two and so never fired on a real retime.
        """
        items = [FakeTimelineItem("C125", 90000, 83, self.PATH_A,
                                  left_offset=0, right_offset=1693,
                                  media_frames=1776, media_fps="50.0")]
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertIn("vitesse 1/2", str(caught.exception))

    def test_an_accelerated_clip_is_refused(self) -> None:
        """The other direction, measured the same day: a clip at 200%.

        The offsets total 444 where the same media spans 888 at speed 1, so
        the ratio comes out 2 exactly -- the formula is not tuned to slow
        motion.
        """
        items = [FakeTimelineItem("C125", 90000, 83, self.PATH_A,
                                  left_offset=0, right_offset=361,
                                  media_frames=1776, media_fps="50.0")]
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertIn("vitesse 2", str(caught.exception))

    def test_a_clip_at_normal_speed_passes(self) -> None:
        """Same rush, same timeline length, untouched speed: measured 888."""
        items = [FakeTimelineItem("C125", 90000, 83, self.PATH_A,
                                  left_offset=0, right_offset=805,
                                  media_frames=1776, media_fps="50.0")]
        flat = flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertEqual(len(flat["segments"]), 1)

    def test_a_trimmed_clip_at_normal_speed_passes(self) -> None:
        """Trimmed both sides, speed 1: offsets still total the whole media.

        Measured the same day by appending source frames 400..799 through the
        API: left 200, span 199, right 489, total 888.
        """
        items = [FakeTimelineItem("C125", 90000, 199, self.PATH_A,
                                  source_start=400, left_offset=200,
                                  right_offset=489, media_frames=1776,
                                  media_fps="50.0")]
        flat = flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertEqual(flat["segments"][0]["source_start"], 400)

    def test_an_unmeasurable_speed_is_refused_not_assumed(self) -> None:
        items = [FakeTimelineItem("C125", 90000, 83, self.PATH_A,
                                  left_offset=0, right_offset=805,
                                  media_frames=1776, media_fps="23.976")]
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertIn("cadence", str(caught.exception))

    def test_a_second_populated_video_track_is_refused(self) -> None:
        items = {1: [FakeTimelineItem("C125", 90000, 216, self.PATH_A)],
                 2: [FakeTimelineItem("cutaway", 90050, 40, self.PATH_B)]}
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", items))
        self.assertIn("pistes vidéo", str(caught.exception))

    def test_an_item_without_a_file_is_refused(self) -> None:
        items = [FakeTimelineItem("Fusion", 90000, 50, "")]
        with self.assertRaises(ResolveBridgeError) as caught:
            flatten_timeline(FakeSourceTimeline("ITW", {1: items}))
        self.assertIn("fichier", str(caught.exception))

    def test_a_fractional_rate_is_refused_rather_than_rounded(self) -> None:
        with self.assertRaises(ResolveBridgeError):
            integral_rate("23.976")
        self.assertEqual(integral_rate("25.0"), 25)


class FakeReadItem(FakeTimelineItem):
    """A timeline item as the read path sees it: framing and Fusion too."""

    def __init__(self, *args: Any, properties=None, fusion_comps: int = 0,
                 no_file_type: str = "", **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.properties = dict(properties or {})
        self.fusion_comps = fusion_comps
        self.no_file_type = no_file_type

    def GetProperty(self, key: str | None = None):
        if key is None:
            return dict(self.properties)
        return self.properties.get(key, "")

    def GetFusionCompCount(self) -> int:
        return self.fusion_comps

    def GetMediaPoolItem(self):
        if not self.path:
            if not self.no_file_type:
                return None
            return FakeResolveClip(**{"File Path": "",
                                      "Type": self.no_file_type})
        return FakeResolveClip(**{"File Path": self.path,
                                  "File Name": self.path.split("/")[-1]})


class FakeTransitionItem:
    """A transition as GetItemListInTrack hands it back, quirks included.

    R10 -- mesuré sur Resolve 20.3.1 le 22 septembre 2026 : un tel élément
    rend None à GetMediaPoolItem, GetSourceStartFrame, GetSourceEndFrame,
    GetLeftOffset et GetRightOffset, son sac de propriétés est vide, et son
    nom est son type (« Cross Dissolve »).
    """

    def __init__(self, name: str, start: int, duration: int) -> None:
        self.name = name
        self.start = start
        self.duration = duration

    def GetName(self) -> str:
        return self.name

    def GetStart(self) -> int:
        return self.start

    def GetEnd(self) -> int:
        return self.start + self.duration

    def GetDuration(self) -> int:
        return self.duration

    def GetSourceStartFrame(self):
        return None

    def GetSourceEndFrame(self):
        return None

    def GetLeftOffset(self):
        return None

    def GetRightOffset(self):
        return None

    def GetMediaPoolItem(self):
        return None

    def GetProperty(self, key: str | None = None):
        return {} if key is None else ""

    def GetFusionCompCount(self) -> int:
        return 0


class FakeReadTimeline(FakeSourceTimeline):
    """Adds what reading a timeline needs and flattening never asked for."""

    def __init__(self, name: str, tracks, audio_tracks=None,
                 width: str = "1080", height: str = "1920",
                 **kwargs: Any) -> None:
        super().__init__(name, tracks, **kwargs)
        self.audio = audio_tracks or {}
        self.width = width
        self.height = height

    def GetTrackCount(self, kind: str) -> int:
        if kind == "audio":
            return max(self.audio) if self.audio else 0
        return max(self.tracks) if self.tracks else 0

    def GetItemListInTrack(self, kind: str, index: int):
        if kind == "audio":
            return self.audio.get(index, [])
        return self.tracks.get(index, []) if kind == "video" else []

    def GetTrackName(self, kind: str, index: int) -> str:
        return f"{'A' if kind == 'audio' else 'V'}{index}"

    def GetSetting(self, key: str) -> str:
        if key == "timelineResolutionWidth":
            return self.width
        if key == "timelineResolutionHeight":
            return self.height
        return super().GetSetting(key)


class ReadTimelineTests(unittest.TestCase):
    """R10 -- le pont relève, il ne juge pas."""

    PATH_A = "/rushes/A044_C125.MP4"
    PATH_B = "/rushes/A044_C126.MP4"

    def timeline(self, **kwargs) -> FakeReadTimeline:
        video = [
            FakeReadItem("C125", 90000, 216, self.PATH_A, source_start=40,
                         properties={"ZoomX": 1.0, "Pan": 0.0}),
            # Un trou de 84 images avant le second plan : parfaitement
            # exprimable par le document, puisque timeline_in est absolu.
            FakeReadItem("C126", 90300, 138, self.PATH_B, source_start=12,
                         properties={"ZoomX": 1.25}),
        ]
        audio = [FakeReadItem("C125", 90000, 216, self.PATH_A,
                              source_start=40)]
        return FakeReadTimeline("MONTAGE", {1: video}, audio_tracks={1: audio},
                                **kwargs)

    def test_reads_video_and_audio_tracks(self) -> None:
        read = read_timeline(self.timeline())
        self.assertEqual(read["name"], "MONTAGE")
        self.assertEqual(read["frame_rate"], 25)
        self.assertEqual([t["kind"] for t in read["tracks"]],
                         ["video", "audio"])
        self.assertEqual([t["index"] for t in read["tracks"]], [1, 1])
        self.assertEqual([t["name"] for t in read["tracks"]], ["V1", "A1"])
        self.assertEqual(len(read["tracks"][0]["clips"]), 2)
        self.assertEqual(len(read["tracks"][1]["clips"]), 1)

    def test_positions_are_relative_to_the_timeline_start(self) -> None:
        # 90000 = 01:00:00:00 à 25 i/s. Sans le décalage, chaque plan
        # arriverait une heure plus loin dans le document.
        clips = read_timeline(self.timeline())["tracks"][0]["clips"]
        self.assertEqual((clips[0]["record_start"], clips[0]["record_end"]),
                         (0, 216))
        self.assertEqual(read_timeline(self.timeline())["origin"], 90000)

    def test_a_gap_is_kept_not_refused(self) -> None:
        # flatten_timeline refuse un trou parce qu'un rendu à plat a besoin de
        # contiguïté ; une relecture n'en a pas besoin.
        clips = read_timeline(self.timeline())["tracks"][0]["clips"]
        self.assertEqual(clips[1]["record_start"], 300)
        self.assertEqual(clips[0]["record_end"], 216)

    def test_the_source_in_point_and_the_file_travel(self) -> None:
        clips = read_timeline(self.timeline())["tracks"][0]["clips"]
        self.assertEqual(clips[0]["source_start"], 40)
        self.assertEqual(clips[0]["path"], self.PATH_A)
        self.assertEqual(clips[0]["duration"], 216)

    def test_framing_properties_travel_as_text(self) -> None:
        # Jamais un flottant dans le manifeste : le lecteur canonique refuse
        # les littéraux flottants, et une valeur se compare en décimal exact.
        clips = read_timeline(self.timeline())["tracks"][0]["clips"]
        self.assertEqual(clips[1]["properties"]["ZoomX"], "1.25")
        for value in clips[0]["properties"].values():
            self.assertIsInstance(value, str)

    def test_a_fusion_composition_is_recorded(self) -> None:
        video = [FakeReadItem("C125", 90000, 216, self.PATH_A,
                              fusion_comps=2)]
        read = read_timeline(FakeReadTimeline("T", {1: video}))
        self.assertEqual(read["tracks"][0]["clips"][0]["fusion_comps"], 2)

    def test_an_item_without_a_file_is_named_not_dropped(self) -> None:
        video = [FakeReadItem("Titre", 90000, 50, "",
                              no_file_type="Compound Clip")]
        read = read_timeline(FakeReadTimeline("T", {1: video}))
        clip = read["tracks"][0]["clips"][0]
        self.assertEqual(clip["path"], "")
        self.assertEqual(clip["no_file_reason"], "Compound Clip")

    def test_an_empty_timeline_is_refused(self) -> None:
        with self.assertRaises(ResolveBridgeError) as caught:
            read_timeline(FakeReadTimeline("VIDE", {1: []}))
        self.assertIn("aucun plan", str(caught.exception))

    # --- R10 : les fondus enchaînés -------------------------------------
    # GetItemListInTrack publie les transitions parmi les plans, nommées
    # d'après leur type, sans média, et chevauchant les deux plans qu'elles
    # raccordent (mesuré sur un vrai montage, Resolve 20.3.1).

    def fondu(self) -> FakeReadTimeline:
        video = [
            FakeReadItem("C125", 90000, 100, self.PATH_A, source_start=40),
            FakeTransitionItem("Cross Dissolve", 90094, 12),
            FakeReadItem("C126", 90100, 100, self.PATH_B, source_start=12),
        ]
        return FakeReadTimeline("FONDU", {1: video})

    def test_a_transition_is_told_apart_from_a_clip(self) -> None:
        self.assertTrue(
            is_transition_item(FakeTransitionItem("Cross Dissolve", 0, 12)))
        self.assertFalse(
            is_transition_item(FakeReadItem("C125", 0, 10, self.PATH_A)))
        # Un clip composé n'a pas de fichier mais a bien un MediaPoolItem :
        # le moteur continue de le refuser nommément, ce n'est pas un fondu.
        self.assertFalse(is_transition_item(
            FakeReadItem("Titre", 0, 10, "", no_file_type="Compound Clip")))
        # Ni un élément sans média dont GetSourceStartFrame rend un entier :
        # le lire comme une transition serait une supposition.
        self.assertFalse(is_transition_item(FakeReadItem("Vide", 0, 10, "")))

    def test_a_transition_lands_in_its_own_list(self) -> None:
        track = read_timeline(self.fondu())["tracks"][0]
        self.assertEqual(len(track["clips"]), 2)
        self.assertEqual(len(track["transitions"]), 1)
        self.assertEqual([c["name"] for c in track["clips"]],
                         ["C125", "C126"])

    def test_the_transition_keeps_its_type_and_its_bounds(self) -> None:
        transition = read_timeline(self.fondu())["tracks"][0]["transitions"][0]
        self.assertEqual(transition["name"], "Cross Dissolve")
        # Relatif au début de la timeline, comme un plan.
        self.assertEqual((transition["record_start"],
                          transition["record_end"]), (94, 106))
        self.assertEqual(transition["duration"], 12)

    def test_the_bridge_does_not_decide_the_alignment(self) -> None:
        # Le pont relève ; c'est le moteur qui lit l'alignement de la place
        # de ces bornes autour de la coupe, parce que lui seul a les plans.
        transition = read_timeline(self.fondu())["tracks"][0]["transitions"][0]
        self.assertEqual(sorted(transition),
                         ["duration", "name", "record_end", "record_start"])

    def test_a_timeline_of_transitions_alone_has_no_clip(self) -> None:
        video = [FakeTransitionItem("Cross Dissolve", 90000, 12)]
        with self.assertRaises(ResolveBridgeError) as caught:
            read_timeline(FakeReadTimeline("T", {1: video}))
        self.assertIn("aucun plan", str(caught.exception))

    def test_a_track_without_a_transition_says_so(self) -> None:
        track = read_timeline(self.timeline())["tracks"][0]
        self.assertEqual(track["transitions"], [])

    def test_a_fractional_rate_never_reaches_the_manifest(self) -> None:
        with self.assertRaises(ResolveBridgeError):
            read_timeline(self.timeline(rate="23.976"))

    def test_an_unreadable_resolution_is_refused(self) -> None:
        with self.assertRaises(ResolveBridgeError) as caught:
            read_timeline(self.timeline(width=""))
        self.assertIn("timelineResolutionWidth", str(caught.exception))

    def test_only_one_key_per_call_still_reads_the_framing(self) -> None:
        # Un build qui n'accepte que GetProperty(clé) : renvoyer un manifeste
        # sans cadrage ferait passer un plan recadré pour un plan brut.
        class OneKeyOnly(FakeReadItem):
            def GetProperty(self, key):
                return self.properties.get(key, "")

        item = OneKeyOnly("C125", 90000, 216, self.PATH_A,
                          properties={"Pan": 100.0})
        read = read_timeline(FakeReadTimeline("T", {1: [item]}))
        self.assertEqual(read["tracks"][0]["clips"][0]["properties"]["Pan"],
                         "100.0")

    def test_a_boolean_property_is_not_python_text(self) -> None:
        item = FakeReadItem("C125", 90000, 216, self.PATH_A,
                            properties={"FlipX": True})
        read = read_timeline(FakeReadTimeline("T", {1: [item]}))
        self.assertEqual(read["tracks"][0]["clips"][0]["properties"]["FlipX"],
                         "1")

    def test_the_named_timeline_carries_the_schema_and_the_blind_spots(self):
        project = FakeReadProject([self.timeline()])
        manifest = read_named_timeline(FakeResolveApp(project), "MONTAGE")
        self.assertEqual(manifest["schema"], TIMELINE_READ_SCHEMA)
        self.assertEqual(manifest["project"], "projet")
        self.assertEqual(manifest["unpublished"], list(UNPUBLISHED_BY_API))
        self.assertEqual(len(manifest["tracks"]), 2)

    def test_an_unknown_timeline_name_is_refused(self) -> None:
        project = FakeReadProject([self.timeline()])
        with self.assertRaises(ResolveBridgeError) as caught:
            read_named_timeline(FakeResolveApp(project), "AUTRE")
        self.assertIn("AUTRE", str(caught.exception))

    def test_the_command_writes_a_manifest_the_engine_can_read(self) -> None:
        project = FakeReadProject([self.timeline()])
        app = FakeResolveApp(project)
        with tempfile.TemporaryDirectory() as scratch:
            destination = Path(scratch) / "timeline.json"
            errors = io.StringIO()
            with mock.patch("sidecar.resolve_bridge.connect",
                            return_value=app), \
                    contextlib.redirect_stderr(errors):
                status = resolve_bridge_main(
                    ["--read-timeline", "MONTAGE", "-o", str(destination)])
            self.assertEqual(status, 0)
            manifest = json.loads(destination.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], TIMELINE_READ_SCHEMA)
        self.assertEqual(manifest["frame_rate"], 25)
        self.assertEqual(manifest["width"], 1080)
        self.assertIn("3 plan(s)", errors.getvalue())

    def test_an_unreachable_timeline_exits_non_zero(self) -> None:
        app = FakeResolveApp(FakeReadProject([self.timeline()]))
        errors = io.StringIO()
        with mock.patch("sidecar.resolve_bridge.connect", return_value=app), \
                contextlib.redirect_stderr(errors):
            status = resolve_bridge_main(["--read-timeline", "AUTRE"])
        self.assertEqual(status, 1)
        self.assertIn("AUTRE", errors.getvalue())

    def test_the_schema_matches_the_importer(self) -> None:
        header = (ROOT / "src" / "ResolveImport.h").read_text(encoding="utf-8")
        self.assertIn(f'"{TIMELINE_READ_SCHEMA}"', header)
        # Le schéma retiré du renvoi ne doit pas être celui de la relecture :
        # un ancien descripteur de montage serait lu comme une timeline.
        self.assertNotEqual(TIMELINE_READ_SCHEMA, TIMELINE_SCHEMA)

    def test_the_property_table_matches_the_importer(self) -> None:
        # Le moteur refuse une clé qu'il ne connaît pas : les deux tables
        # doivent rester la même, sinon toute timeline serait refusée.
        source = (ROOT / "src" / "ResolveImport.cc").read_text(
            encoding="utf-8")
        table = source.split("ResolveItemPropertyDefaults() {", 1)[1]
        table = table.split("return defaults;", 1)[0]
        for key in FRAMING_PROPERTIES:
            self.assertIn(f'"{key}"', table)


class FakeReadProject:
    def __init__(self, timelines) -> None:
        self.timelines = timelines

    def GetName(self) -> str:
        return "projet"

    def GetTimelineCount(self) -> int:
        return len(self.timelines)

    def GetTimelineByIndex(self, index: int):
        return self.timelines[index - 1]


class RemapTests(unittest.TestCase):
    def flattening(self) -> dict[str, Any]:
        return {
            "name": "ITW", "frame_rate": 25, "duration": 354,
            "segments": [
                {"record_start": 0, "record_end": 216, "source_start": 0,
                 "path": "/rushes/C125.braw", "filename": "C125.braw"},
                {"record_start": 216, "record_end": 354, "source_start": 0,
                 "path": "/rushes/C126.braw", "filename": "C126.braw"},
            ],
        }

    def montage(self, start: int, end: int, record: int = 0) -> dict[str, Any]:
        return {"schema": TIMELINE_SCHEMA, "name": "MONTAGE",
                "frame_rate": {"num": 25, "den": 1},
                "clips": [{"path": "/renders/ITW.mov", "filename": "ITW.mov",
                           "start_frame": start, "end_frame": end,
                           "video_layer": 0, "record_frame": record,
                           "with_audio": True}]}

    def test_a_cut_inside_one_rush_keeps_its_length(self) -> None:
        out = remap_to_sources(self.montage(50, 100), self.flattening())
        self.assertEqual(len(out["clips"]), 1)
        clip = out["clips"][0]
        self.assertEqual(clip["path"], "/rushes/C125.braw")
        self.assertEqual((clip["start_frame"], clip["end_frame"]), (50, 100))

    def test_a_cut_across_a_join_comes_back_as_two_plans(self) -> None:
        # 200-250 straddles the join at 216: 16 frames of C125 then 34 of C126,
        # and the second must sit 16 frames later on the montage.
        out = remap_to_sources(self.montage(200, 250, record=1000),
                               self.flattening())
        self.assertEqual(len(out["clips"]), 2)
        first, second = out["clips"]
        self.assertEqual(first["path"], "/rushes/C125.braw")
        self.assertEqual((first["start_frame"], first["end_frame"]), (200, 216))
        self.assertEqual(first["record_frame"], 1000)
        self.assertEqual(second["path"], "/rushes/C126.braw")
        self.assertEqual((second["start_frame"], second["end_frame"]), (0, 34))
        self.assertEqual(second["record_frame"], 1016)

    def test_the_total_length_survives_the_split(self) -> None:
        out = remap_to_sources(self.montage(200, 250), self.flattening())
        total = sum(c["end_frame"] - c["start_frame"] for c in out["clips"])
        self.assertEqual(total, 50)

    def test_a_rate_mismatch_is_refused(self) -> None:
        montage = self.montage(0, 50)
        montage["frame_rate"] = {"num": 50, "den": 1}
        with self.assertRaises(ResolveBridgeError) as caught:
            remap_to_sources(montage, self.flattening())
        self.assertIn("déplacerait les coupes", str(caught.exception))

    def test_a_cut_beyond_the_render_is_refused(self) -> None:
        with self.assertRaises(ResolveBridgeError) as caught:
            remap_to_sources(self.montage(300, 400), self.flattening())
        self.assertIn("sort du rendu", str(caught.exception))


class FakeRenderProject:
    """Project that already has jobs of its own waiting in the queue."""

    def __init__(self, timelines) -> None:
        self.timelines = timelines
        self.foreign = ["job_deja_en_file_1", "job_deja_en_file_2"]
        self.created: list[str] = []
        self.started: list[str] | None = None
        self.settings: list[dict[str, Any]] = []
        self.current = None
        self.codec = None

    def GetTimelineCount(self) -> int:
        return len(self.timelines)

    def GetTimelineByIndex(self, index: int):
        return self.timelines[index - 1]

    def SetCurrentTimeline(self, timeline) -> bool:
        self.current = timeline
        return True

    def SetCurrentRenderFormatAndCodec(self, fmt, codec) -> bool:
        self.codec = (fmt, codec)
        return True

    def SetRenderSettings(self, settings) -> bool:
        self.settings.append(settings)
        return True

    def AddRenderJob(self) -> str:
        job = f"job_{len(self.created) + 1}"
        self.created.append(job)
        return job

    def StartRendering(self, jobs) -> bool:
        self.started = list(jobs)
        return True

    def GetMediaPool(self):
        return None


class RenderTests(unittest.TestCase):
    def app(self):
        timelines = [FakeSourceTimeline("ITW_Exemple", {1: []}),
                     FakeSourceTimeline("ITW_Exemple_B", {1: []})]
        project = FakeRenderProject(timelines)
        return FakeResolveApp(project), project

    def flattening(self) -> dict[str, Any]:
        return {"schema": FLATTEN_SCHEMA, "project": "P", "timelines": [
            {"name": "ITW_Exemple", "frame_rate": 25, "duration": 10,
             "segments": []},
            {"name": "ITW_Exemple_B", "frame_rate": 25, "duration": 20,
             "segments": []}]}

    def test_only_its_own_jobs_are_started(self) -> None:
        # StartRendering() with no argument runs the whole queue. The
        # operator's two waiting jobs must not be launched by our render.
        app, project = self.app()
        with tempfile.TemporaryDirectory() as target:
            queued = run_renders(app, self.flattening(), target)
        self.assertEqual([e["job"] for e in queued], ["job_1", "job_2"])
        self.assertEqual(project.started, ["job_1", "job_2"])
        for job in project.foreign:
            self.assertNotIn(job, project.started)

    def test_every_timeline_renders_all_of_its_frames(self) -> None:
        # A stray in/out left in a timeline would otherwise render a fragment
        # and desynchronise every frame number in the flattening map.
        app, project = self.app()
        with tempfile.TemporaryDirectory() as target:
            run_renders(app, self.flattening(), target)
        self.assertEqual(len(project.settings), 2)
        for settings in project.settings:
            self.assertTrue(settings["SelectAllFrames"])
            self.assertTrue(settings["ExportAudio"])
        self.assertEqual(project.codec, ("mov", "ProRes422LT"))

    def test_the_render_carries_the_timeline_name(self) -> None:
        plan = plan_render({"name": "ITW_Exemple_B"}, "/tmp/out")
        self.assertEqual(plan["CustomName"], "ITW_Exemple_B")
        self.assertEqual(plan["TargetDir"], "/tmp/out")


# R9 -- docs/roadmap/REEL-2026-09.md. Le renvoi par XML FCP7 : ce que le pont
# ajoute au fichier du moteur (le timecode de départ de chaque rush, seul
# critère d'appariement de Resolve) et ce qu'il relit après l'import.
def xmeml(clips, transitions=(), tracks=1) -> str:
    """Un xmeml v5 minimal, de la forme que `--export-resolve-xml` écrit."""
    body = ""
    for layer in range(tracks):
        body += "<track>"
        for clip in clips:
            if clip.get("layer", 0) != layer:
                continue
            body += (
                f'<clipitem id="{clip["id"]}"><name>{clip["name"]}</name>'
                f'<start>{clip["start"]}</start><end>{clip["end"]}</end>'
                f'<in>{clip.get("in", 0)}</in>'
                f'<out>{clip.get("out", 0)}</out>')
            if clip.get("defined", True):
                body += (
                    f'<file id="{clip["file"]}"><name>{clip["name"]}</name>'
                    f'<pathurl>file://{clip["path"]}</pathurl>'
                    '<timecode><string>99:00:00:00</string>'
                    '<rate><timebase>25</timebase></rate></timecode></file>')
            else:
                body += f'<file id="{clip["file"]}"/>'
            body += "</clipitem>"
        for transition in transitions:
            if transition.get("layer", 0) != layer:
                continue
            body += ('<transitionitem><start>%d</start><end>%d</end>'
                     '<alignment>center</alignment></transitionitem>'
                     % (transition["start"], transition["end"]))
        body += "</track>"
    return ('<?xml version="1.0" encoding="UTF-8"?><xmeml version="5">'
            '<sequence><name>MONTAGE</name><media><video>' + body +
            "</video></media></sequence></xmeml>")


class FakeXmlItem:
    """Un plan (ou une transition) de timeline, vu par l'API."""

    def __init__(self, start: int, media=None, origin: int = 90000) -> None:
        self.start = origin + start
        self.media = media

    def GetStart(self) -> int:
        return self.start

    def GetMediaPoolItem(self):
        return self.media


class FakeXmlTimeline:
    def __init__(self, tracks, origin: int = 90000) -> None:
        self.tracks = tracks
        self.origin = origin

    def GetStartFrame(self) -> int:
        return self.origin

    def GetTrackCount(self, kind: str) -> int:
        return len(self.tracks) if kind == "video" else 0

    def GetItemListInTrack(self, kind: str, index: int):
        if kind != "video":
            return []
        return self.tracks[index - 1]

    def GetName(self) -> str:
        return "CM_R9"


class FakeXmlMediaPool:
    def __init__(self, root, timeline) -> None:
        self.root = root
        self.timeline = timeline
        self.imported = None
        self.options = None

    def GetRootFolder(self):
        return self.root

    def ImportTimelineFromFile(self, path, options):
        self.imported = path
        self.options = options
        # Resolve classe la timeline importée dans le chutier : le Media Pool
        # gagne exactement cet élément, et aucun rush.
        self.root.clips.append(FakeResolveClip(**{
            "File Path": "", "Clip Name": options["timelineName"],
            "Type": "Timeline"}))
        return self.timeline


class FakeXmlProject:
    def __init__(self, pool, timelines=()) -> None:
        self.pool = pool
        self.timelines = list(timelines)

    def GetMediaPool(self):
        return self.pool

    def GetTimelineCount(self) -> int:
        return len(self.timelines)

    def GetTimelineByIndex(self, index):
        return self.timelines[index - 1]

    def GetName(self):
        return "projet"


class ResolveXmlSendTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)

    def tree(self, text: str):
        import xml.etree.ElementTree as ElementTree
        return ElementTree.ElementTree(ElementTree.fromstring(text))

    def montage(self) -> str:
        return xmeml(
            [
                {"id": "c1", "file": "file-1", "name": "A.MP4",
                 "path": "/rushes/A.MP4", "start": 0, "end": 100, "in": 50,
                 "out": 150},
                {"id": "c2", "file": "file-2", "name": "B.MP4",
                 "path": "/rushes/B.MP4", "start": 100, "end": 200},
                {"id": "c3", "file": "file-1", "name": "A.MP4",
                 "path": "/rushes/A.MP4", "start": 200, "end": 260,
                 "defined": False},
            ],
            transitions=[{"start": 94, "end": 106}])

    def test_the_xml_gives_one_file_per_rush_and_the_order(self) -> None:
        files, tracks = read_xml_timeline(self.tree(self.montage()))
        # Le troisième plan relit le premier rush : décrit une fois, référencé
        # ensuite par son id, comme l'écrit le moteur.
        self.assertEqual(sorted(files), ["file-1", "file-2"])
        self.assertEqual(len(tracks), 1)
        self.assertEqual([clip["start"] for clip in tracks[0]["clips"]],
                         [0, 100, 200])
        self.assertEqual(tracks[0]["transitions"], 1)

    def test_a_file_referenced_but_never_described_is_refused(self) -> None:
        text = xmeml([{"id": "c1", "file": "file-9", "name": "A.MP4",
                       "path": "/rushes/A.MP4", "start": 0, "end": 10,
                       "defined": False}])
        with self.assertRaises(ResolveBridgeError) as refusal:
            read_xml_timeline(self.tree(text))
        self.assertIn("file-9", str(refusal.exception))

    def test_the_bridge_stamps_the_start_timecode_of_each_rush(self) -> None:
        # Resolve apparie un <file> par son timecode de départ et par rien
        # d'autre (mesuré le 22 septembre) : le moteur ne le connaît pas, donc
        # il écrit un marqueur que le pont remplace ici.
        tree = self.tree(self.montage())
        files, _ = read_xml_timeline(tree)
        stamped = stamp_xml_timecodes(
            files, lambda path: ("10:59:48:00", 50)
            if path.endswith("A.MP4") else ("10:52:50:10", 25))
        self.assertEqual(stamped["file-1"], "/rushes/A.MP4")
        self.assertEqual(files["file-1"].findtext("timecode/string"),
                         "10:59:48:00")
        self.assertEqual(files["file-1"].findtext("timecode/rate/timebase"),
                         "50")
        self.assertEqual(files["file-2"].findtext("timecode/string"),
                         "10:52:50:10")

    def test_a_rush_absent_from_the_pool_is_refused_by_name(self) -> None:
        files, _ = read_xml_timeline(self.tree(self.montage()))
        with self.assertRaises(ResolveBridgeError) as refusal:
            stamp_xml_timecodes(
                files, lambda path: None if path.endswith("B.MP4")
                else ("10:59:48:00", 50))
        self.assertIn("B.MP4", str(refusal.exception))

    def test_two_rushes_sharing_a_start_timecode_are_refused(self) -> None:
        # Resolve n'a alors aucun moyen de les distinguer, et poserait l'un
        # pour l'autre sans le dire.
        files, _ = read_xml_timeline(self.tree(self.montage()))
        with self.assertRaises(ResolveBridgeError) as refusal:
            stamp_xml_timecodes(files, lambda path: ("10:00:00:00", 25))
        self.assertIn("timecode", str(refusal.exception))

    def test_every_media_pool_folder_is_searched(self) -> None:
        leaf = FakeResolveFolder("JOUR 2")
        rushes = FakeResolveFolder("RUSHES", folders=[leaf])
        root = FakeResolveFolder("Master", folders=[rushes])
        self.assertEqual([folder.GetName() for folder in
                          media_pool_folders(root)],
                         ["Master", "RUSHES", "JOUR 2"])

    def placed(self, paths):
        return [FakeXmlItem(start, FakeResolveClip(**{"File Path": path}))
                for start, path in paths]

    def test_a_clip_landed_on_another_rush_is_caught(self) -> None:
        _, tracks = read_xml_timeline(self.tree(self.montage()))
        paths = {"file-1": "/rushes/A.MP4", "file-2": "/rushes/B.MP4"}
        timeline = FakeXmlTimeline([self.placed([
            (0, "/rushes/A.MP4"), (100, "/rushes/AUTRE.MP4"),
            (200, "/rushes/A.MP4")]) + [FakeXmlItem(94)]])
        with self.assertRaises(ResolveBridgeError) as refusal:
            verify_imported_timeline(timeline, tracks, paths)
        self.assertIn("AUTRE.MP4", str(refusal.exception))

    def test_a_clip_left_offline_is_caught(self) -> None:
        _, tracks = read_xml_timeline(self.tree(self.montage()))
        paths = {"file-1": "/rushes/A.MP4", "file-2": "/rushes/B.MP4"}
        timeline = FakeXmlTimeline([self.placed([
            (0, "/rushes/A.MP4"), (200, "/rushes/A.MP4")]) +
            [FakeXmlItem(94), FakeXmlItem(100)]])
        with self.assertRaises(ResolveBridgeError) as refusal:
            verify_imported_timeline(timeline, tracks, paths)
        self.assertIn("hors ligne", str(refusal.exception))

    def test_a_clip_at_another_position_is_caught(self) -> None:
        _, tracks = read_xml_timeline(self.tree(self.montage()))
        paths = {"file-1": "/rushes/A.MP4", "file-2": "/rushes/B.MP4"}
        timeline = FakeXmlTimeline([self.placed([
            (0, "/rushes/A.MP4"), (101, "/rushes/B.MP4"),
            (200, "/rushes/A.MP4")]) + [FakeXmlItem(94)]])
        with self.assertRaises(ResolveBridgeError) as refusal:
            verify_imported_timeline(timeline, tracks, paths)
        self.assertIn("101", str(refusal.exception))

    def app(self, timeline, clips=None):
        pool = FakeXmlMediaPool(
            FakeResolveFolder("Master", clips=clips or [
                FakeResolveClip(**{"File Path": "/rushes/A.MP4",
                                   "File Name": "A.MP4",
                                   "Start TC": "10:59:48:00", "FPS": "50.0"}),
                FakeResolveClip(**{"File Path": "/rushes/B.MP4",
                                   "File Name": "B.MP4",
                                   "Start TC": "10:52:50:10", "FPS": "25.0"}),
            ]),
            timeline)
        return FakeResolveApp(FakeXmlProject(pool)), pool

    def test_the_whole_trip_stamps_imports_and_checks(self) -> None:
        source = self.scratch / "montage.xml"
        source.write_text(self.montage(), encoding="utf-8")
        timeline = FakeXmlTimeline([self.placed([
            (0, "/rushes/A.MP4"), (100, "/rushes/B.MP4"),
            (200, "/rushes/A.MP4")]) + [FakeXmlItem(94)]])
        app, pool = self.app(timeline)
        summary = send_timeline_xml(app, str(source), "CM_R9",
                                    str(self.scratch / "copies"))
        self.assertIn("3 plan(s)", summary)
        self.assertIn("Media Pool +1", summary)
        self.assertIn("1 transition(s)", summary)
        self.assertFalse(pool.options["importSourceClips"])
        self.assertEqual([folder.GetName()
                          for folder in pool.options["sourceClipsFolders"]],
                         ["Master"])
        # Le fichier réellement donné à Resolve est la copie complétée, pas
        # celui du moteur : celui-ci garde son marqueur.
        self.assertNotEqual(pool.imported, str(source))
        stamped = Path(pool.imported).read_text(encoding="utf-8")
        self.assertIn("10:59:48:00", stamped)
        self.assertNotIn("99:00:00:00", stamped)
        self.assertIn("<!DOCTYPE xmeml>", stamped)
        self.assertIn("99:00:00:00", source.read_text(encoding="utf-8"))

    def test_a_name_already_taken_is_refused_before_importing(self) -> None:
        source = self.scratch / "montage.xml"
        source.write_text(self.montage(), encoding="utf-8")
        app, pool = self.app(FakeXmlTimeline([[]]))
        existing = FakeXmlTimeline([[]])
        app.project.timelines = [existing]
        with self.assertRaises(ResolveBridgeError) as refusal:
            send_timeline_xml(app, str(source), "CM_R9",
                              str(self.scratch / "copies"))
        self.assertIn("CM_R9", str(refusal.exception))
        self.assertIsNone(pool.imported)

    def test_send_xml_refuses_a_srt(self) -> None:
        # Mesuré : le xmeml ne porte pas de piste de sous-titres, et une
        # timeline importée arrive pleine, si bien qu'AppendToTimeline
        # décalerait chaque cue de la longueur du montage.
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = resolve_bridge_main(
                ["--send-xml", "/absent.xml", "--srt", "/absent.srt"])
        self.assertEqual(code, 1)
        self.assertIn("sous-titres", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
