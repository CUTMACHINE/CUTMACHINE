"""SN3 -- docs/roadmap/SON-2026-10.md. Sound rendering for the Planner.

A subprocess boundary to tools/cutmachine-sound/cli.mjs, the way binary.py is
one to the engine: a ditty, a project, a seed and settings in; the stem's path
and the renderer's JSON report out. The planner never reckons a time: the cuts
come from the project (SN2), the instants from the renderer.
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .binary import BinaryError, CutmachineBinary


class SoundError(RuntimeError):
    pass


@dataclass(frozen=True)
class SoundRender:
    # The stem, a 24-bit WAV whose BWF time reference is the timeline's start.
    stem: Path
    # The renderer's report as it printed it: sha256, peaks, seed, inputs,
    # loops, messages, timeReference, the edit read.
    report: dict[str, Any]
    # One WAV per loop, when stems were asked for.
    loop_stems: tuple[Path, ...] = ()


_SETTING_NAME = re.compile(r"^[A-Za-z_$][\w$]*$")
_DEFAULT_CLI = (Path(__file__).resolve().parents[1] / "tools" /
                "cutmachine-sound" / "cli.mjs")


def _setting_argument(name: str, value: Any) -> str:
    if not isinstance(name, str) or not _SETTING_NAME.match(name):
        raise SoundError(f"setting {name!r} is not a ditty input name")
    # bool is an int to Python and neither a number nor a word to the
    # renderer's --set: refused rather than spelled one way or the other.
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise SoundError(
            f"setting {name!r} must be a number or a string, not "
            f"{type(value).__name__}")
    if isinstance(value, float) and not math.isfinite(value):
        raise SoundError(f"setting {name!r} is not a finite number")
    if isinstance(value, str) and "\n" in value:
        raise SoundError(f"setting {name!r} holds a line break")
    return f"{name}={value}"


def _package_of(project: Path) -> Path | None:
    """The .cutmachine-project folder a project path names, if any."""
    if project.is_dir():
        return project
    if project.name == "project.cutmachine.json":
        return project.parent
    return None


class SoundRenderer:
    def __init__(self, node: str | os.PathLike[str] | None = None,
                 cli: str | os.PathLike[str] | None = None) -> None:
        found = node or os.environ.get("CUTMACHINE_NODE") or shutil.which("node")
        self.node = str(found) if found else None
        self.cli = str(cli or _DEFAULT_CLI)

    def _run(self, arguments: list[str], timeout: float) -> dict[str, Any]:
        if self.node is None:
            raise SoundError(
                "Node 18 or later is needed to render sound: install it, or "
                "name it in CUTMACHINE_NODE")
        try:
            completed = subprocess.run(
                [self.node, self.cli, *arguments], check=False, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise SoundError(f"the renderer did not answer within {timeout:g} s") from exc
        except OSError as exc:
            raise SoundError(f"unable to execute {self.node}: {exc}") from exc
        if completed.returncode != 0:
            detail = completed.stderr.strip()
            try:
                payload = json.loads(detail.splitlines()[-1])
                detail = payload.get("error", detail)
            except (json.JSONDecodeError, IndexError, AttributeError):
                pass
            raise SoundError(detail or f"the renderer exited with status {completed.returncode}")
        try:
            report = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            raise SoundError(f"the renderer printed no JSON report: {completed.stdout[:200]!r}") from exc
        if not isinstance(report, dict):
            raise SoundError("the renderer's report must be a JSON object")
        return report

    @staticmethod
    def _montage(project: Path, timeline: str | None, track: str | None,
                 start: str | None) -> list[str]:
        arguments = ["--project", str(project)]
        for flag, value in (("--timeline", timeline), ("--track", track),
                            ("--start", start)):
            if value is not None:
                arguments += [flag, value]
        return arguments

    def edit(self, project: str | os.PathLike[str], *, timeline: str | None = None,
             track: str | None = None, start: str | None = None) -> dict[str, Any]:
        """The edit a ditty sees: shots and cuts with their ids, times and frames."""
        return self._run(
            ["edit", *self._montage(Path(project), timeline, track, start)], 60)

    def render(self, ditty: str | os.PathLike[str], project: str | os.PathLike[str], *,
               seed: int = 1, settings: Mapping[str, Any] | None = None,
               out: str | os.PathLike[str] | None = None,
               timeline: str | None = None, track: str | None = None,
               start: str | None = None, sample_rate: int | None = None,
               tail: float | None = None, loop_stems: str | os.PathLike[str] | None = None,
               partition: str | os.PathLike[str] | None = None,
               timeout: float = 300) -> SoundRender:
        """Render `ditty` against a timeline of `project`.

        With `partition` (SN5), `ditty` is an instrument set and the partition
        says what plays when. Without `out`, the stem goes to the project
        package's Sound/ folder, named after the ditty (and the partition)
        and the seed.
        """
        ditty = Path(ditty)
        project = Path(project)
        if not ditty.is_file():
            raise SoundError(f"no ditty at {ditty}")
        if isinstance(seed, bool) or not isinstance(seed, int):
            raise SoundError("the seed must be a whole number")
        if out is None:
            package = _package_of(project)
            if package is None:
                raise SoundError("say where the stem goes (out): the project is not a package folder")
            name = ditty.stem if partition is None else f"{Path(partition).stem}-{ditty.stem}"
            out = package / "Sound" / f"{name}-seed{seed}.wav"
        out = Path(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        arguments = ["render", str(ditty), *self._montage(project, timeline, track, start),
                     "--seed", str(seed), "--out", str(out), "--timeout", f"{timeout:g}"]
        for name, value in (settings or {}).items():
            arguments += ["--set", _setting_argument(name, value)]
        if sample_rate is not None:
            if isinstance(sample_rate, bool) or not isinstance(sample_rate, int) or sample_rate <= 0:
                raise SoundError("the sample rate must be a whole number of samples per second")
            arguments += ["--sr", str(sample_rate)]
        if tail is not None:
            arguments += ["--tail", f"{float(tail):g}"]
        if loop_stems is not None:
            arguments += ["--stems", str(loop_stems)]
        if partition is not None:
            if not Path(partition).is_file():
                raise SoundError(f"no partition at {partition}")
            arguments += ["--partition", str(partition), "--triggers"]
        report = self._run(arguments, timeout + 30)
        # Node's path.resolve, like os.path.abspath, does not follow links.
        stem = Path(os.path.abspath(out))
        if report.get("ok") is not True or Path(report.get("out", "")) != stem:
            raise SoundError(f"the renderer's report does not name {stem}: {report!r}"[:400])
        if not stem.is_file():
            raise SoundError(f"the renderer reported {stem} but wrote no file there")
        return SoundRender(
            stem=stem, report=report,
            loop_stems=tuple(Path(s["file"]) for s in report.get("stems", [])))


def place_stem(binary: CutmachineBinary, project: str | os.PathLike[str],
               stem: str | os.PathLike[str], *, track_id: str | None = None,
               start_timecode: str | None = None) -> dict[str, Any]:
    """SN4 -- the stem into the library, then laid where its BWF timecode
    says (place_sound_stem): one undo for the laying. Returns the tool's
    `placed`, with the stem's `media_id`."""
    project = Path(project)
    package = _package_of(project)
    if package is None:
        raise SoundError("place_stem needs the project's package folder or its project.cutmachine.json")
    stem = Path(os.path.abspath(stem))
    try:
        binary.ingest(project, stem)
        media_id = None
        for media in binary.library(project):
            path = Path(media.get("path", ""))
            if not path.is_absolute():
                path = package / path
            if Path(os.path.abspath(path)) == stem:
                media_id = media.get("id")
        if not media_id:
            raise SoundError(f"{stem} is not in the library after its ingest")
        arguments: dict[str, Any] = {"media_id": media_id}
        if track_id is not None:
            arguments["track_id"] = track_id
        if start_timecode is not None:
            arguments["start_timecode"] = start_timecode
        result = binary.call_tool(project, "place_sound_stem", arguments)
    except BinaryError as exc:
        raise SoundError(str(exc)) from exc
    placed = result.get("placed")
    if not isinstance(placed, dict):
        raise SoundError("place_sound_stem said nothing of what it laid")
    return {**placed, "media_id": media_id}
