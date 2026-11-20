#!/usr/bin/env python3
"""Normalize and score frame-based word alignments without media dependencies."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
import statistics
import struct
import sys
import unicodedata
import wave
from collections import Counter, defaultdict
from decimal import Decimal, InvalidOperation, ROUND_CEILING, ROUND_FLOOR
from fractions import Fraction
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1
FILLER_WORDS = {"euh", "heu", "hum", "hm", "mh", "ben", "bah", "beh", "hein"}


class BenchmarkError(ValueError):
    pass


def load_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BenchmarkError(f"{path}: {exc}") from exc


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
    temporary.replace(path)


def write_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_text(value, encoding="utf-8")
    temporary.replace(path)


def parse_rate(value: str) -> tuple[int, int]:
    fields = value.split("/", maxsplit=1)
    try:
        numerator = int(fields[0])
        denominator = int(fields[1]) if len(fields) == 2 else 1
    except ValueError as exc:
        raise BenchmarkError(f"invalid source rate '{value}'") from exc
    if numerator <= 0 or denominator <= 0:
        raise BenchmarkError("source rate components must be positive")
    return numerator, denominator


def decimal_value(value: Any, field: str) -> Decimal:
    try:
        result = Decimal(str(value))
    except InvalidOperation as exc:
        raise BenchmarkError(f"{field} is not a decimal number") from exc
    if not result.is_finite():
        raise BenchmarkError(f"{field} must be finite")
    return result


def frame_floor(seconds: Decimal, rate: tuple[int, int]) -> int:
    value = seconds * Decimal(rate[0]) / Decimal(rate[1])
    return int(value.to_integral_value(rounding=ROUND_FLOOR))


def frame_ceil(seconds: Decimal, rate: tuple[int, int]) -> int:
    value = seconds * Decimal(rate[0]) / Decimal(rate[1])
    return int(value.to_integral_value(rounding=ROUND_CEILING))


def milliseconds_for_frames(frames: int, rate: tuple[int, int]) -> float:
    return float(frames) * 1000 * rate[1] / rate[0]


def rounded_milliseconds_for_frames(frames: int, rate: tuple[int, int]) -> int:
    value = Fraction(frames * 1000 * rate[1], rate[0])
    if value < 0:
        raise BenchmarkError("silence duration cannot be negative")
    return (2 * value.numerator + value.denominator) // (2 * value.denominator)


def folded_word(text: str) -> str:
    folded = unicodedata.normalize("NFKD", text.casefold())
    letters = "".join(character for character in folded if character.isalpha())
    if not letters:
        return ""
    collapsed = [letters[0]]
    for character in letters[1:]:
        if character != collapsed[-1]:
            collapsed.append(character)
    return "".join(collapsed)


def is_filler(text: str) -> bool:
    return folded_word(text) in FILLER_WORDS


def quantize_words(
    entries: Iterable[tuple[str, Decimal, Decimal]], rate: tuple[int, int]
) -> list[dict[str, Any]]:
    """Apply CUTMACHINE's outward rounding and non-overlap policy."""
    words: list[dict[str, Any]] = []
    pending: list[str] = []
    previous_raw_start: Decimal | None = None
    for text, start_seconds, end_seconds in entries:
        text = text.strip()
        if not text:
            continue
        if start_seconds < 0 or end_seconds < start_seconds:
            raise BenchmarkError(f"invalid interval for word '{text}'")
        if previous_raw_start is not None and start_seconds < previous_raw_start:
            raise BenchmarkError("word intervals are not in chronological order")
        previous_raw_start = start_seconds
        start = frame_floor(start_seconds, rate)
        end = frame_ceil(end_seconds, rate)
        if words:
            start = max(start, words[-1]["frame_end"])
        if end <= start:
            pending.append(text)
            continue
        if pending:
            text = " ".join(pending + [text])
            pending.clear()
        words.append(
            {
                "text": text,
                "frame_start": start,
                "frame_end": end,
                "confidence": None,
                "filler_word": is_filler(text),
                "silence_after_frames": 0,
                "silence_after_ms": 0,
                "speaker_id": None,
            }
        )
    if pending:
        if not words:
            raise BenchmarkError("no word owns a representable source frame")
        words[-1]["text"] += " " + " ".join(pending)
        words[-1]["filler_word"] = is_filler(words[-1]["text"])
    for index, word in enumerate(words):
        next_start = (
            words[index + 1]["frame_start"]
            if index + 1 < len(words)
            else word["frame_end"]
        )
        silence = max(0, next_start - word["frame_end"])
        word["silence_after_frames"] = silence
        word["silence_after_ms"] = rounded_milliseconds_for_frames(silence, rate)
    return words


def manifest(
    *,
    sample_id: str,
    media_id: str,
    rate: tuple[int, int],
    method: str,
    version: str,
    model: str,
    timing_provenance: str,
    reference_text_sha256: str | None,
    wall_time_ms: int,
    audio_duration_frames: int,
    words: list[dict[str, Any]],
    speaker_id: str | None,
) -> dict[str, Any]:
    for word in words:
        word["speaker_id"] = speaker_id
    result = {
        "schema_version": SCHEMA_VERSION,
        "sample_id": sample_id,
        "media_id": media_id,
        "language": "fr",
        "source_rate": {"num": rate[0], "den": rate[1]},
        "alignment": {
            "method": method,
            "version": version,
            "model": model,
            "timing_provenance": timing_provenance,
            "reference_text_sha256": reference_text_sha256,
        },
        "processing": {
            "wall_time_ms": wall_time_ms,
            "audio_duration_frames": audio_duration_frames,
        },
        "words": words,
    }
    validate_manifest(result)
    return result


def _require_integer(value: Any, field: str, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BenchmarkError(f"{field} must be an integer")
    if minimum is not None and value < minimum:
        raise BenchmarkError(f"{field} must be at least {minimum}")
    return value


def validate_manifest(value: Any) -> None:
    if not isinstance(value, dict):
        raise BenchmarkError("manifest root must be an object")
    if value.get("schema_version") != SCHEMA_VERSION:
        raise BenchmarkError(f"schema_version must be {SCHEMA_VERSION}")
    for field in ("sample_id", "media_id", "language"):
        if not isinstance(value.get(field), str) or not value[field]:
            raise BenchmarkError(f"{field} must be a non-empty string")
    source_rate = value.get("source_rate")
    if not isinstance(source_rate, dict):
        raise BenchmarkError("source_rate must be an object")
    rate = (
        _require_integer(source_rate.get("num"), "source_rate.num", 1),
        _require_integer(source_rate.get("den"), "source_rate.den", 1),
    )
    alignment = value.get("alignment")
    if not isinstance(alignment, dict):
        raise BenchmarkError("alignment must be an object")
    for field in ("method", "version", "model", "timing_provenance"):
        if not isinstance(alignment.get(field), str) or not alignment[field]:
            raise BenchmarkError(f"alignment.{field} must be a non-empty string")
    if alignment["method"] not in {"gold", "cutmachine", "mfa", "nemo", "qwen"}:
        raise BenchmarkError("alignment.method is not supported")
    reference_hash = alignment.get("reference_text_sha256")
    if reference_hash is not None and (
        not isinstance(reference_hash, str)
        or len(reference_hash) != 64
        or any(character not in "0123456789abcdef" for character in reference_hash)
    ):
        raise BenchmarkError("alignment.reference_text_sha256 is invalid")
    processing = value.get("processing")
    if not isinstance(processing, dict):
        raise BenchmarkError("processing must be an object")
    _require_integer(processing.get("wall_time_ms"), "processing.wall_time_ms", 0)
    duration = _require_integer(
        processing.get("audio_duration_frames"),
        "processing.audio_duration_frames",
        1,
    )
    words = value.get("words")
    if not isinstance(words, list):
        raise BenchmarkError("words must be an array")
    previous_end = 0
    for index, word in enumerate(words):
        path = f"words[{index}]"
        if not isinstance(word, dict):
            raise BenchmarkError(f"{path} must be an object")
        if not isinstance(word.get("text"), str) or not word["text"]:
            raise BenchmarkError(f"{path}.text must be a non-empty string")
        start = _require_integer(word.get("frame_start"), f"{path}.frame_start", 0)
        end = _require_integer(word.get("frame_end"), f"{path}.frame_end", 1)
        if end <= start:
            raise BenchmarkError(f"{path} must occupy at least one frame")
        if start < previous_end:
            raise BenchmarkError(f"{path} overlaps the previous word")
        if end > duration:
            raise BenchmarkError(f"{path} exceeds the audio duration")
        previous_end = end
        confidence = word.get("confidence")
        if confidence is not None and (
            isinstance(confidence, bool)
            or not isinstance(confidence, (int, float))
            or not math.isfinite(confidence)
            or confidence < 0
            or confidence > 1
        ):
            raise BenchmarkError(f"{path}.confidence must be null or in [0, 1]")
        if not isinstance(word.get("filler_word"), bool):
            raise BenchmarkError(f"{path}.filler_word must be a boolean")
        silence_frames = _require_integer(
            word.get("silence_after_frames"), f"{path}.silence_after_frames", 0
        )
        silence_ms = _require_integer(
            word.get("silence_after_ms"), f"{path}.silence_after_ms", 0
        )
        expected_silence = max(
            0,
            words[index + 1].get("frame_start", end) - end
            if index + 1 < len(words)
            else 0,
        )
        if silence_frames != expected_silence:
            raise BenchmarkError(f"{path}.silence_after_frames is inconsistent")
        if silence_ms != rounded_milliseconds_for_frames(silence_frames, rate):
            raise BenchmarkError(f"{path}.silence_after_ms is inconsistent")
        if word.get("speaker_id") is not None and not isinstance(
            word.get("speaker_id"), str
        ):
            raise BenchmarkError(f"{path}.speaker_id must be null or a string")


def rational_time_to_frame(time: Any, rate: tuple[int, int], field: str) -> int:
    if not isinstance(time, dict):
        raise BenchmarkError(f"{field} must be an object")
    value = _require_integer(time.get("value"), f"{field}.value")
    denominator = _require_integer(time.get("rate"), f"{field}.rate", 1)
    frames = Fraction(value * rate[0], denominator * rate[1])
    if frames.denominator != 1:
        raise BenchmarkError(f"{field} is not exactly on the source frame grid")
    return frames.numerator


def cutmachine_manifest(args: argparse.Namespace) -> dict[str, Any]:
    cache = load_json(args.input)
    source_rate = cache.get("source_rate")
    if not isinstance(source_rate, dict):
        raise BenchmarkError("CUTMACHINE cache has no source_rate")
    rate = (
        _require_integer(source_rate.get("num"), "source_rate.num", 1),
        _require_integer(source_rate.get("den"), "source_rate.den", 1),
    )
    words = []
    for index, word in enumerate(cache.get("words", [])):
        start = rational_time_to_frame(word.get("start"), rate, f"words[{index}].start")
        end = rational_time_to_frame(word.get("end"), rate, f"words[{index}].end")
        words.append(
            {
                "text": word.get("text"),
                "frame_start": start,
                "frame_end": end,
                "confidence": None,
                "filler_word": is_filler(word.get("text", "")),
                "silence_after_frames": 0,
                "silence_after_ms": 0,
                "speaker_id": None,
            }
        )
    for index, word in enumerate(words):
        next_start = (
            words[index + 1]["frame_start"]
            if index + 1 < len(words)
            else word["frame_end"]
        )
        silence = max(0, next_start - word["frame_end"])
        word["silence_after_frames"] = silence
        word["silence_after_ms"] = rounded_milliseconds_for_frames(silence, rate)
    provenance = "cache-v1-unknown"
    if cache.get("speech_aligned"):
        provenance += "+speech-envelope-snap"
    return manifest(
        sample_id=args.sample_id,
        media_id=cache.get("media_id", args.media_id),
        rate=rate,
        method="cutmachine",
        version=args.version,
        model=cache.get("whisper_model", "unknown"),
        timing_provenance=provenance,
        reference_text_sha256=hash_reference_text(word["text"] for word in words),
        wall_time_ms=args.wall_time_ms,
        audio_duration_frames=args.audio_duration_frames,
        words=words,
        speaker_id=args.speaker_id,
    )


def mfa_entries(
    path: Path, requested_tier: str | None
) -> list[tuple[str, Decimal, Decimal]]:
    data = load_json(path)
    tiers = data.get("tiers") if isinstance(data, dict) else None
    if not isinstance(tiers, dict):
        raise BenchmarkError("MFA JSON has no praatIO tiers object")
    if requested_tier:
        names = [requested_tier] if requested_tier in tiers else []
    else:
        names = [
            name
            for name in tiers
            if name.casefold().endswith("words") or name.casefold() == "word"
        ]
    if len(names) != 1:
        raise BenchmarkError(
            "MFA word tier is ambiguous; pass --tier (candidates: "
            + ", ".join(sorted(tiers))
            + ")"
        )
    tier = tiers[names[0]]
    entries = tier.get("entries") if isinstance(tier, dict) else None
    if not isinstance(entries, list):
        raise BenchmarkError(f"MFA tier '{names[0]}' has no entries array")
    result = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, list) or len(entry) != 3:
            raise BenchmarkError(f"MFA entry {index} must be [start, end, label]")
        result.append(
            (
                str(entry[2]),
                decimal_value(entry[0], f"entries[{index}].start"),
                decimal_value(entry[1], f"entries[{index}].end"),
            )
        )
    return result


def praat_binary_tiers(
    path: Path,
) -> dict[str, list[tuple[float, float | None, str]]]:
    """Tiers of a Praat binary TextGrid (`ooBinaryFile`), by name.

    Interval tiers give (start, end, label), point tiers (time, None, label).
    Numbers are big-endian; a label is a 16-bit length and Latin-1 bytes, or
    0xFFFF, a length and UTF-16 code units.
    """
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BenchmarkError(f"{path}: {exc}") from exc
    position = 0

    def take(count: int) -> bytes:
        nonlocal position
        chunk = data[position : position + count]
        if len(chunk) != count:
            raise BenchmarkError(f"{path}: truncated binary TextGrid")
        position += count
        return chunk

    def number() -> float:
        return struct.unpack(">d", take(8))[0]

    def integer() -> int:
        return struct.unpack(">i", take(4))[0]

    def label() -> str:
        length = struct.unpack(">H", take(2))[0]
        if length == 0xFFFF:
            length = struct.unpack(">H", take(2))[0]
            return take(2 * length).decode("utf-16-be")
        return take(length).decode("latin-1")

    if take(12) != b"ooBinaryFile" or take(take(1)[0]) != b"TextGrid":
        raise BenchmarkError(f"{path}: not a Praat binary TextGrid")
    number(), number()
    tiers: dict[str, list[tuple[float, float | None, str]]] = {}
    if take(1) != b"\x01":
        return tiers
    for _ in range(integer()):
        kind = take(take(1)[0]).decode("ascii")
        name = label()
        number(), number()
        entries: list[tuple[float, float | None, str]] = []
        for _ in range(integer()):
            if kind == "IntervalTier":
                start, end = number(), number()
                entries.append((start, end, label()))
            else:
                entries.append((number(), None, label()))
        tiers[name] = entries
    if position != len(data):
        raise BenchmarkError(f"{path}: {len(data) - position} trailing bytes")
    return tiers


def ctm_entries(path: Path) -> list[tuple[str, Decimal, Decimal]]:
    result = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise BenchmarkError(f"{path}: {exc}") from exc
    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) < 5:
            raise BenchmarkError(
                f"{path}:{line_number}: expected at least five CTM fields"
            )
        start = decimal_value(fields[2], f"{path}:{line_number}: start")
        duration = decimal_value(fields[3], f"{path}:{line_number}: duration")
        result.append((fields[4], start, start + duration))
    return result


def hash_reference_text(texts: Iterable[str]) -> str:
    text = " ".join(" ".join(texts).split())
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def alignment_manifest(args: argparse.Namespace) -> dict[str, Any]:
    rate = parse_rate(args.source_rate)
    if args.command == "normalize-mfa":
        entries = mfa_entries(args.input, args.tier)
        method = "mfa"
        provenance = "mfa-word-tier"
    elif args.command == "normalize-qwen":
        entries = ctm_entries(args.input)
        method = "qwen"
        provenance = "qwen3-forced-aligner-word-ctm"
    else:
        entries = ctm_entries(args.input)
        method = "nemo"
        provenance = "nfa-word-ctm"
    words = quantize_words(entries, rate)
    try:
        reference_text = args.reference_text.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise BenchmarkError(f"{args.reference_text}: {exc}") from exc
    return manifest(
        sample_id=args.sample_id,
        media_id=args.media_id,
        rate=rate,
        method=method,
        version=args.version,
        model=args.model,
        timing_provenance=provenance,
        reference_text_sha256=hash_reference_text([reference_text]),
        wall_time_ms=args.wall_time_ms,
        audio_duration_frames=args.audio_duration_frames,
        words=words,
        speaker_id=args.speaker_id,
    )


def rounded_mean(values: list[int]) -> int:
    if not values:
        raise BenchmarkError("cannot average an empty value list")
    return round(Fraction(sum(values), len(values)))


def ensemble_manifests(
    predictions: list[dict[str, Any]], wall_time_ms: int
) -> dict[str, Any]:
    """Average frame boundaries and resolve expansion overlaps exactly."""
    if len(predictions) < 2:
        raise BenchmarkError("an ensemble needs at least two predictions")
    for prediction in predictions:
        validate_manifest(prediction)
    first = predictions[0]
    for prediction in predictions[1:]:
        for field in ("sample_id", "media_id", "source_rate", "language"):
            if prediction[field] != first[field]:
                raise BenchmarkError(f"ensemble predictions differ on {field}")
        if prediction["processing"]["audio_duration_frames"] != first[
            "processing"
        ]["audio_duration_frames"]:
            raise BenchmarkError("ensemble predictions have different durations")
        if prediction["alignment"]["reference_text_sha256"] != first[
            "alignment"
        ]["reference_text_sha256"]:
            raise BenchmarkError("ensemble predictions used different reference text")
        if len(prediction["words"]) != len(first["words"]):
            raise BenchmarkError("ensemble predictions have different word counts")
        for index, word in enumerate(prediction["words"]):
            if word["text"] != first["words"][index]["text"]:
                raise BenchmarkError(
                    f"ensemble predictions differ on words[{index}].text"
                )

    result = copy.deepcopy(first)
    duration = result["processing"]["audio_duration_frames"]
    boundaries = []
    for index in range(len(first["words"])):
        start = rounded_mean(
            [prediction["words"][index]["frame_start"] for prediction in predictions]
        ) - 1
        end = rounded_mean(
            [prediction["words"][index]["frame_end"] for prediction in predictions]
        ) + 1
        start = max(0, min(start, duration - 1))
        end = max(start + 1, min(end, duration))
        boundaries.append([start, end])

    for index in range(len(boundaries) - 1):
        current = boundaries[index]
        following = boundaries[index + 1]
        if current[1] <= following[0]:
            continue
        split = round(Fraction(current[1] + following[0], 2))
        split = max(current[0] + 1, min(split, following[1] - 1))
        current[1] = split
        following[0] = split

    for word, (start, end) in zip(result["words"], boundaries):
        word["frame_start"] = start
        word["frame_end"] = end
    rate = (result["source_rate"]["num"], result["source_rate"]["den"])
    for index, word in enumerate(result["words"]):
        next_start = (
            result["words"][index + 1]["frame_start"]
            if index + 1 < len(result["words"])
            else word["frame_end"]
        )
        silence = max(0, next_start - word["frame_end"])
        word["silence_after_frames"] = silence
        word["silence_after_ms"] = rounded_milliseconds_for_frames(silence, rate)
    result["alignment"] = {
        "method": "nemo",
        "version": "ensemble-v1",
        "model": "mean(" + ", ".join(
            prediction["alignment"]["model"] for prediction in predictions
        ) + ")",
        "timing_provenance": "mean-boundaries+expand-1-frame+midpoint-overlap",
        "reference_text_sha256": first["alignment"]["reference_text_sha256"],
    }
    result["processing"] = {
        "wall_time_ms": wall_time_ms,
        "audio_duration_frames": duration,
    }
    validate_manifest(result)
    return result


def srt_for_manifest(value: dict[str, Any]) -> str:
    validate_manifest(value)
    rate = (value["source_rate"]["num"], value["source_rate"]["den"])

    def timecode(frame: int) -> str:
        milliseconds = Fraction(frame * 1000 * rate[1], rate[0])
        if milliseconds.denominator != 1:
            raise BenchmarkError("SRT timestamps are not exact integer milliseconds")
        total = milliseconds.numerator
        hours, total = divmod(total, 3_600_000)
        minutes, total = divmod(total, 60_000)
        seconds, remainder = divmod(total, 1000)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d},{remainder:03d}"

    lines = []
    for index, word in enumerate(value["words"], start=1):
        lines.extend(
            [
                str(index),
                f"{timecode(word['frame_start'])} --> {timecode(word['frame_end'])}",
                word["text"],
                "",
            ]
        )
    return "\n".join(lines)


def comparison_key(text: str) -> str:
    normalized = unicodedata.normalize("NFKC", text.casefold())
    return "".join(character for character in normalized if character.isalnum())


def matched_word_indices(
    gold_words: list[dict[str, Any]], predicted_words: list[dict[str, Any]]
) -> list[tuple[int, int]]:
    gold = [comparison_key(word["text"]) for word in gold_words]
    predicted = [comparison_key(word["text"]) for word in predicted_words]
    rows = len(gold) + 1
    columns = len(predicted) + 1
    lengths = [[0] * columns for _ in range(rows)]
    for left in range(len(gold) - 1, -1, -1):
        for right in range(len(predicted) - 1, -1, -1):
            if gold[left] and gold[left] == predicted[right]:
                lengths[left][right] = lengths[left + 1][right + 1] + 1
            else:
                lengths[left][right] = max(
                    lengths[left + 1][right], lengths[left][right + 1]
                )
    pairs = []
    left = right = 0
    while left < len(gold) and right < len(predicted):
        if gold[left] and gold[left] == predicted[right]:
            pairs.append((left, right))
            left += 1
            right += 1
        elif lengths[left + 1][right] >= lengths[left][right + 1]:
            left += 1
        else:
            right += 1
    return pairs


def percentile(values: list[int], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def score_pair(gold: dict[str, Any], predicted: dict[str, Any]) -> dict[str, Any]:
    validate_manifest(gold)
    validate_manifest(predicted)
    if gold["sample_id"] != predicted["sample_id"]:
        raise BenchmarkError("gold and prediction sample_id differ")
    if gold["source_rate"] != predicted["source_rate"]:
        raise BenchmarkError("gold and prediction source_rate differ")
    pairs = matched_word_indices(gold["words"], predicted["words"])
    start_errors = []
    end_errors = []
    signed_start = []
    signed_end = []
    within = 0
    for gold_index, predicted_index in pairs:
        expected = gold["words"][gold_index]
        observed = predicted["words"][predicted_index]
        start_delta = observed["frame_start"] - expected["frame_start"]
        end_delta = observed["frame_end"] - expected["frame_end"]
        start_errors.append(abs(start_delta))
        end_errors.append(abs(end_delta))
        signed_start.append(start_delta)
        signed_end.append(end_delta)
        if abs(start_delta) <= 1 and abs(end_delta) <= 1:
            within += 1
    rate = (gold["source_rate"]["num"], gold["source_rate"]["den"])
    boundary_errors = start_errors + end_errors
    start_errors_ms = [milliseconds_for_frames(value, rate) for value in start_errors]
    end_errors_ms = [milliseconds_for_frames(value, rate) for value in end_errors]
    signed_start_ms = [milliseconds_for_frames(value, rate) for value in signed_start]
    signed_end_ms = [milliseconds_for_frames(value, rate) for value in signed_end]
    boundary_errors_ms = start_errors_ms + end_errors_ms
    boundary_mae_frames = statistics.fmean(boundary_errors) if boundary_errors else None
    to_ms = lambda value: (
        None if value is None else milliseconds_for_frames(value, rate)
    )
    processing = predicted["processing"]
    duration_seconds = Fraction(
        processing["audio_duration_frames"] * rate[1], rate[0]
    )
    seconds_per_audio_minute = (
        float(Fraction(processing["wall_time_ms"], 1000) * 60 / duration_seconds)
        if duration_seconds > 0
        else None
    )
    return {
        "sample_id": gold["sample_id"],
        "method": predicted["alignment"]["method"],
        "model": predicted["alignment"]["model"],
        "gold_words": len(gold["words"]),
        "predicted_words": len(predicted["words"]),
        "matched_words": len(pairs),
        "lexical_coverage_percent": 100 * len(pairs) / len(gold["words"])
        if gold["words"]
        else None,
        "boundary_mae_ms": to_ms(boundary_mae_frames),
        "start_mae_ms": to_ms(statistics.fmean(start_errors) if start_errors else None),
        "end_mae_ms": to_ms(statistics.fmean(end_errors) if end_errors else None),
        "start_bias_ms": to_ms(
            statistics.fmean(signed_start) if signed_start else None
        ),
        "end_bias_ms": to_ms(statistics.fmean(signed_end) if signed_end else None),
        "boundary_p50_ms": to_ms(percentile(boundary_errors, 0.50)),
        "boundary_p95_ms": to_ms(percentile(boundary_errors, 0.95)),
        "within_one_frame_percent": 100 * within / len(pairs) if pairs else None,
        "within_one_frame_words": within,
        "wall_time_ms": processing["wall_time_ms"],
        "audio_duration_frames": processing["audio_duration_frames"],
        "processing_seconds_per_audio_minute": seconds_per_audio_minute,
        "_start_errors": start_errors,
        "_end_errors": end_errors,
        "_start_errors_ms": start_errors_ms,
        "_end_errors_ms": end_errors_ms,
        "_signed_start_ms": signed_start_ms,
        "_signed_end_ms": signed_end_ms,
        "_boundary_errors_ms": boundary_errors_ms,
        "_within": within,
        "_rate": rate,
        "_duration_seconds": duration_seconds,
    }


def agreement_from_score(
    left: dict[str, Any], right: dict[str, Any], score: dict[str, Any]
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "status": "agreement_not_accuracy",
        "sample_id": left["sample_id"],
        "left": {
            "method": left["alignment"]["method"],
            "model": left["alignment"]["model"],
            "words": len(left["words"]),
        },
        "right": {
            "method": right["alignment"]["method"],
            "model": right["alignment"]["model"],
            "words": len(right["words"]),
        },
        "matched_words": score["matched_words"],
        "lexical_overlap_percent_of_left": score["lexical_coverage_percent"],
        "boundary_mean_absolute_disagreement_ms": score["boundary_mae_ms"],
        "start_mean_absolute_disagreement_ms": score["start_mae_ms"],
        "end_mean_absolute_disagreement_ms": score["end_mae_ms"],
        "right_minus_left_start_bias_ms": score["start_bias_ms"],
        "right_minus_left_end_bias_ms": score["end_bias_ms"],
        "boundary_disagreement_p50_ms": score["boundary_p50_ms"],
        "boundary_disagreement_p95_ms": score["boundary_p95_ms"],
        "both_boundaries_within_one_frame_percent": score[
            "within_one_frame_percent"
        ],
        "accuracy": None,
        "winner": None,
    }


def agreement_pair(left: dict[str, Any], right: dict[str, Any]) -> dict[str, Any]:
    """Measure two alignments' disagreement without treating either as truth."""
    return agreement_from_score(left, right, score_pair(left, right))


def levenshtein_alignment(
    reference: list[str], hypothesis: list[str]
) -> list[tuple[str, int | None, int | None]]:
    """Return a deterministic word-error alignment.

    Diagonal moves win ties so substitutions are not reported as an arbitrary
    insertion/deletion pair. This keeps Resolve ASR reports stable.
    """
    rows = len(reference) + 1
    columns = len(hypothesis) + 1
    distances = [[0] * columns for _ in range(rows)]
    for row in range(rows):
        distances[row][0] = row
    for column in range(columns):
        distances[0][column] = column
    for row in range(1, rows):
        for column in range(1, columns):
            substitution = reference[row - 1] != hypothesis[column - 1]
            distances[row][column] = min(
                distances[row - 1][column] + 1,
                distances[row][column - 1] + 1,
                distances[row - 1][column - 1] + substitution,
            )

    result: list[tuple[str, int | None, int | None]] = []
    row = len(reference)
    column = len(hypothesis)
    while row or column:
        if row and column:
            substitution = reference[row - 1] != hypothesis[column - 1]
            if (
                distances[row][column]
                == distances[row - 1][column - 1] + substitution
            ):
                result.append(
                    (
                        "substitution" if substitution else "correct",
                        row - 1,
                        column - 1,
                    )
                )
                row -= 1
                column -= 1
                continue
        if row and distances[row][column] == distances[row - 1][column] + 1:
            result.append(("deletion", row - 1, None))
            row -= 1
        else:
            result.append(("insertion", None, column - 1))
            column -= 1
    result.reverse()
    return result


def score_resolve_captions(
    gold: dict[str, Any], capture: dict[str, Any]
) -> dict[str, Any]:
    """Score Resolve text and only the word boundaries it exposes natively."""
    validate_manifest(gold)
    if not isinstance(capture, dict):
        raise BenchmarkError("Resolve capture root must be an object")
    if capture.get("source_rate") != gold["source_rate"]:
        raise BenchmarkError("gold and Resolve source_rate differ")
    duration = _require_integer(
        capture.get("audio_duration_frames"),
        "audio_duration_frames",
        1,
    )
    if duration != gold["processing"]["audio_duration_frames"]:
        raise BenchmarkError("gold and Resolve audio durations differ")
    wall_time_ms = _require_integer(
        capture.get("wall_time_ms"), "wall_time_ms", 0
    )
    version = capture.get("resolve_version")
    if not isinstance(version, str) or not version:
        raise BenchmarkError("resolve_version must be a non-empty string")
    cues = capture.get("cues")
    if not isinstance(cues, list):
        raise BenchmarkError("Resolve capture has no cues array")

    hypothesis: list[str] = []
    token_origins: list[tuple[int, int, str]] = []
    previous_end = 0
    for cue_index, cue in enumerate(cues):
        path = f"cues[{cue_index}]"
        if not isinstance(cue, dict):
            raise BenchmarkError(f"{path} must be an object")
        text = cue.get("text")
        if not isinstance(text, str) or not text.split():
            raise BenchmarkError(f"{path}.text must contain at least one word")
        start = _require_integer(cue.get("frame_start"), f"{path}.frame_start", 0)
        end = _require_integer(cue.get("frame_end"), f"{path}.frame_end", 1)
        if end <= start:
            raise BenchmarkError(f"{path} must occupy at least one frame")
        if start < previous_end:
            raise BenchmarkError(f"{path} overlaps the previous cue")
        if end > duration:
            raise BenchmarkError(f"{path} exceeds the audio duration")
        previous_end = end
        tokens = text.split()
        for token in tokens:
            hypothesis.append(comparison_key(token))
            token_origins.append((cue_index, len(tokens), token))

    reference = [comparison_key(word["text"]) for word in gold["words"]]
    alignment = levenshtein_alignment(reference, hypothesis)
    counts = Counter(operation for operation, _, _ in alignment)
    word_errors = (
        counts["substitution"] + counts["deletion"] + counts["insertion"]
    )
    rate = (gold["source_rate"]["num"], gold["source_rate"]["den"])
    start_errors: list[int] = []
    end_errors: list[int] = []
    signed_starts: list[int] = []
    signed_ends: list[int] = []
    within = 0
    matches = []
    for operation, gold_index, hypothesis_index in alignment:
        if operation != "correct":
            continue
        assert gold_index is not None and hypothesis_index is not None
        cue_index, cue_word_count, token = token_origins[hypothesis_index]
        if cue_word_count != 1:
            continue
        cue = cues[cue_index]
        expected = gold["words"][gold_index]
        start_delta = cue["frame_start"] - expected["frame_start"]
        end_delta = cue["frame_end"] - expected["frame_end"]
        start_errors.append(abs(start_delta))
        end_errors.append(abs(end_delta))
        signed_starts.append(start_delta)
        signed_ends.append(end_delta)
        if abs(start_delta) <= 1 and abs(end_delta) <= 1:
            within += 1
        matches.append(
            {
                "gold_index": gold_index,
                "hypothesis_index": hypothesis_index,
                "text": token,
                "start_delta_frames": start_delta,
                "end_delta_frames": end_delta,
            }
        )

    boundary_errors = start_errors + end_errors
    to_ms = lambda value: (
        None if value is None else milliseconds_for_frames(value, rate)
    )
    duration_seconds = Fraction(duration * rate[1], rate[0])
    error_details = []
    for operation, gold_index, hypothesis_index in alignment:
        if operation == "correct":
            continue
        error_details.append(
            {
                "operation": operation,
                "reference": gold["words"][gold_index]["text"]
                if gold_index is not None
                else None,
                "hypothesis": token_origins[hypothesis_index][2]
                if hypothesis_index is not None
                else None,
            }
        )
    return {
        "schema_version": 1,
        "sample_id": gold["sample_id"],
        "engine": {
            "name": "DaVinci Resolve Auto Caption",
            "version": version,
            "language": capture.get("language"),
            "settings": capture.get("settings"),
        },
        "reference_words": len(reference),
        "hypothesis_words": len(hypothesis),
        "word_errors": word_errors,
        "word_error_rate_percent": 100 * word_errors / len(reference)
        if reference
        else None,
        "correct_words": counts["correct"],
        "substitutions": counts["substitution"],
        "deletions": counts["deletion"],
        "insertions": counts["insertion"],
        "exact_lexical_coverage_percent": 100
        * counts["correct"]
        / len(reference)
        if reference
        else None,
        "native_cues": len(cues),
        "native_single_word_cues": sum(
            len(cue["text"].split()) == 1 for cue in cues
        ),
        "native_multiword_cues": sum(
            len(cue["text"].split()) > 1 for cue in cues
        ),
        "native_single_word_timing": {
            "matched_words": len(matches),
            "boundary_mae_ms": to_ms(
                statistics.fmean(boundary_errors) if boundary_errors else None
            ),
            "start_mae_ms": to_ms(
                statistics.fmean(start_errors) if start_errors else None
            ),
            "end_mae_ms": to_ms(
                statistics.fmean(end_errors) if end_errors else None
            ),
            "start_bias_ms": to_ms(
                statistics.fmean(signed_starts) if signed_starts else None
            ),
            "end_bias_ms": to_ms(
                statistics.fmean(signed_ends) if signed_ends else None
            ),
            "boundary_p50_ms": to_ms(percentile(boundary_errors, 0.50)),
            "boundary_p95_ms": to_ms(percentile(boundary_errors, 0.95)),
            "within_one_frame_percent": 100 * within / len(matches)
            if matches
            else None,
        },
        "wall_time_ms": wall_time_ms,
        "processing_seconds_per_audio_minute": float(
            Fraction(wall_time_ms, 1000) * 60 / duration_seconds
        ),
        "errors": error_details,
        "native_single_word_matches": matches,
    }


def public_score(score: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in score.items() if not key.startswith("_")}


def aggregate_scores(scores: list[dict[str, Any]]) -> dict[str, Any]:
    if not scores:
        raise BenchmarkError("cannot aggregate an empty score set")
    start_errors = [value for score in scores for value in score["_start_errors_ms"]]
    end_errors = [value for score in scores for value in score["_end_errors_ms"]]
    signed_start = [value for score in scores for value in score["_signed_start_ms"]]
    signed_end = [value for score in scores for value in score["_signed_end_ms"]]
    boundary_errors = start_errors + end_errors
    matched = sum(score["matched_words"] for score in scores)
    gold_words = sum(score["gold_words"] for score in scores)
    wall_time = sum(score["wall_time_ms"] for score in scores)
    duration_frames = sum(score["audio_duration_frames"] for score in scores)
    duration_seconds = sum((score["_duration_seconds"] for score in scores), Fraction())
    same_rate = all(score["_rate"] == scores[0]["_rate"] for score in scores)
    return {
        "method": scores[0]["method"],
        "model": scores[0]["model"],
        "samples": len(scores),
        "gold_words": gold_words,
        "matched_words": matched,
        "lexical_coverage_percent": 100 * matched / gold_words if gold_words else None,
        "boundary_mae_ms": statistics.fmean(boundary_errors)
        if boundary_errors
        else None,
        "start_mae_ms": statistics.fmean(start_errors) if start_errors else None,
        "end_mae_ms": statistics.fmean(end_errors) if end_errors else None,
        "start_bias_ms": statistics.fmean(signed_start) if signed_start else None,
        "end_bias_ms": statistics.fmean(signed_end) if signed_end else None,
        "boundary_p50_ms": percentile(boundary_errors, 0.50),
        "boundary_p95_ms": percentile(boundary_errors, 0.95),
        "within_one_frame_percent": 100
        * sum(score["_within"] for score in scores)
        / matched
        if matched
        else None,
        "wall_time_ms": wall_time,
        "audio_duration_frames": duration_frames if same_rate else None,
        "audio_duration_ms": float(duration_seconds * 1000),
        "processing_seconds_per_audio_minute": float(
            Fraction(wall_time, 1000) * 60 / duration_seconds
        )
        if duration_seconds > 0
        else None,
    }


def discover_manifests(directory: Path) -> list[dict[str, Any]]:
    manifests = []
    for path in sorted(directory.rglob("*.json")):
        value = load_json(path)
        try:
            validate_manifest(value)
        except BenchmarkError:
            continue
        manifests.append(value)
    return manifests


def manifests_by_sample(directory: Path, label: str) -> dict[str, dict[str, Any]]:
    result = {}
    for value in discover_manifests(directory):
        sample_id = value["sample_id"]
        if sample_id in result:
            raise BenchmarkError(f"duplicate {label} manifest for '{sample_id}'")
        result[sample_id] = value
    if not result:
        raise BenchmarkError(f"{label} directory contains no valid manifests")
    return result


def agreement_directories(
    left_directory: Path, right_directory: Path
) -> dict[str, Any]:
    left = manifests_by_sample(left_directory, "left")
    right = manifests_by_sample(right_directory, "right")
    if left.keys() != right.keys():
        raise BenchmarkError("left and right directories contain different samples")
    scores = []
    details = []
    for sample_id in sorted(left):
        score = score_pair(left[sample_id], right[sample_id])
        scores.append(score)
        details.append(agreement_from_score(left[sample_id], right[sample_id], score))
    aggregate = aggregate_scores(scores)
    left_words = sum(len(value["words"]) for value in left.values())
    right_words = sum(len(value["words"]) for value in right.values())
    return {
        "schema_version": 1,
        "status": "agreement_not_accuracy",
        "samples": len(scores),
        "left_words": left_words,
        "right_words": right_words,
        "matched_words": aggregate["matched_words"],
        "lexical_overlap_percent_of_left": 100
        * aggregate["matched_words"]
        / left_words,
        "boundary_mean_absolute_disagreement_ms": aggregate["boundary_mae_ms"],
        "start_mean_absolute_disagreement_ms": aggregate["start_mae_ms"],
        "end_mean_absolute_disagreement_ms": aggregate["end_mae_ms"],
        "right_minus_left_start_bias_ms": aggregate["start_bias_ms"],
        "right_minus_left_end_bias_ms": aggregate["end_bias_ms"],
        "boundary_disagreement_p50_ms": aggregate["boundary_p50_ms"],
        "boundary_disagreement_p95_ms": aggregate["boundary_p95_ms"],
        "both_boundaries_within_one_frame_percent": aggregate[
            "within_one_frame_percent"
        ],
        "accuracy": None,
        "winner": None,
        "details": details,
    }


# Envelope junctions: a proxy, not a score. Where the audio holds a real
# pause between two words both aligners matched, the envelope places the end
# of the first word at the pause's first silent frame and the start of the
# next at its first voiced frame, on the engine's own voice line (a frame is
# voiced when a 20 ms window it touches reaches 25 % of the sample's 90th
# percentile, SpeechOnset.h and WordBoundaries.h). Each aligner is measured
# against that edge. The edge has its own bias -- a soft fricative falls under
# the line, a breath rises above it -- but the same one for both aligners,
# and it is the only reference neither of them produced. Only pause
# junctions count: inside continuous speech the envelope has no edge.
#
# Two choices can favour one aligner, so both are arguments: who locates the
# junctions (`select`; a junction found with one aligner's frames is one that
# aligner got roughly right), and the voice line (`speech_ratio_percent`; a
# higher line hears the voice later, which favours an aligner that starts
# late). Measured on 31 October 2026, the end edges did not depend on either;
# the start edges depended on the line.
ENVELOPE_WINDOWS_PER_SECOND = 50
ENVELOPE_SPEECH_PERCENTILE = 90
ENVELOPE_SPEECH_RATIO_PERCENT = 25


def envelope_window_powers(path: Path) -> tuple[int, list[int]]:
    """Return the sample rate and each whole window's sum of squares."""
    try:
        with wave.open(str(path), "rb") as handle:
            if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
                raise BenchmarkError(f"{path}: expected mono 16-bit PCM")
            sample_rate = handle.getframerate()
            frames = handle.readframes(handle.getnframes())
    except (OSError, wave.Error) as exc:
        raise BenchmarkError(f"{path}: {exc}") from exc
    if sample_rate % ENVELOPE_WINDOWS_PER_SECOND:
        raise BenchmarkError(f"{path}: sample rate has no whole 20 ms window")
    window = sample_rate // ENVELOPE_WINDOWS_PER_SECOND
    samples = memoryview(frames).cast("h")
    powers = []
    for start in range(0, len(samples) - window + 1, window):
        powers.append(sum(value * value for value in samples[start : start + window]))
    return sample_rate, powers


def voiced_frames(
    powers: list[int],
    rate: tuple[int, int],
    frames: int,
    speech_ratio_percent: int = ENVELOPE_SPEECH_RATIO_PERCENT,
) -> list[bool]:
    ranked = sorted(powers)
    if not ranked:
        raise BenchmarkError("audio holds no whole envelope window")
    if not 0 < speech_ratio_percent <= 100:
        raise BenchmarkError("speech ratio must lie in 1..100 percent")
    rank = max(1, math.ceil(ENVELOPE_SPEECH_PERCENTILE * len(ranked) / 100))
    speech_power = ranked[rank - 1]
    # The ratio is in amplitude and windows are compared by their sum of
    # squares, so it is squared: the voice line stays exact in integers.
    line = speech_ratio_percent * speech_ratio_percent * speech_power
    wps = ENVELOPE_WINDOWS_PER_SECOND
    result = []
    for frame in range(frames):
        first = frame * rate[1] * wps // rate[0]
        last = -(-(frame + 1) * rate[1] * wps // rate[0])
        result.append(
            any(
                10000 * powers[index] >= line
                for index in range(first, min(last, len(powers)))
            )
        )
    return result


def pause_runs(
    voiced: list[bool], low: int, high: int, minimum_frames: int
) -> list[tuple[int, int]]:
    """Silent runs inside [low, high) with voice on both sides."""
    low = max(1, low)
    high = min(len(voiced) - 1, high)
    runs = []
    frame = low
    while frame < high:
        if voiced[frame]:
            frame += 1
            continue
        start = frame
        while frame < high and not voiced[frame]:
            frame += 1
        if (
            frame < high
            and voiced[start - 1]
            and voiced[frame]
            and frame - start >= minimum_frames
        ):
            runs.append((start, frame))
    return runs


def side_summary(errors: list[int], rate: tuple[int, int]) -> dict[str, Any]:
    if not errors:
        return {"count": 0}
    absolute = [abs(value) for value in errors]
    return {
        "count": len(errors),
        "mean_absolute_frames": statistics.fmean(absolute),
        "mean_absolute_ms": milliseconds_for_frames(1, rate)
        * statistics.fmean(absolute),
        "signed_bias_ms": milliseconds_for_frames(1, rate) * statistics.fmean(errors),
        "within_one_frame_percent": 100
        * sum(1 for value in absolute if value <= 1)
        / len(absolute),
        "p90_absolute_frames": percentile(absolute, 0.9),
    }


def envelope_junctions(
    left: dict[str, Any],
    right: dict[str, Any],
    voiced: list[bool],
    minimum_pause_frames: int = 4,
    search_frames: int = 10,
    select: str = "both",
) -> dict[str, Any]:
    for value in (left, right):
        validate_manifest(value)
    if left["source_rate"] != right["source_rate"]:
        raise BenchmarkError("manifests do not share a frame rate")
    if select not in ("both", "left", "right"):
        raise BenchmarkError("select must be both, left or right")
    rate = (left["source_rate"]["num"], left["source_rate"]["den"])
    lw, rw = left["words"], right["words"]
    pairs = matched_word_indices(lw, rw)
    errors: dict[str, dict[str, list[int]]] = {
        side: {"end": [], "start": []} for side in ("left", "right")
    }
    junctions = ambiguous = continuous = 0
    for (li, ri), (lj, rj) in zip(pairs, pairs[1:]):
        if lj != li + 1 or rj != ri + 1:
            continue
        locators = {
            "both": [(lw, li, lj), (rw, ri, rj)],
            "left": [(lw, li, lj)],
            "right": [(rw, ri, rj)],
        }[select]
        low = min(w[i]["frame_end"] for w, i, _ in locators) - search_frames
        high = max(w[j]["frame_start"] for w, _, j in locators) + search_frames
        # The pause must sit after the first word starts and before the next
        # one ends, for each locating aligner: otherwise the pause of a
        # neighbouring junction is read as this one's.
        after = max(w[i]["frame_start"] for w, i, _ in locators)
        before = min(w[j]["frame_end"] for w, _, j in locators)
        runs = [
            run
            for run in pause_runs(voiced, low, high, minimum_pause_frames)
            if run[0] > after and run[1] < before
        ]
        if not runs:
            continuous += 1
            continue
        if len(runs) > 1:
            ambiguous += 1
            continue
        junctions += 1
        pause_start, pause_end = runs[0]
        errors["left"]["end"].append(lw[li]["frame_end"] - pause_start)
        errors["right"]["end"].append(rw[ri]["frame_end"] - pause_start)
        errors["left"]["start"].append(lw[lj]["frame_start"] - pause_end)
        errors["right"]["start"].append(rw[rj]["frame_start"] - pause_end)
    return {
        "schema_version": 1,
        "status": "envelope_proxy_not_accuracy",
        "sample_id": left["sample_id"],
        "minimum_pause_frames": minimum_pause_frames,
        "select": select,
        "pause_junctions": junctions,
        "ambiguous_junctions": ambiguous,
        "continuous_junctions": continuous,
        "left": {
            "method": left["alignment"]["method"],
            "model": left["alignment"]["model"],
            "end": side_summary(errors["left"]["end"], rate),
            "start": side_summary(errors["left"]["start"], rate),
        },
        "right": {
            "method": right["alignment"]["method"],
            "model": right["alignment"]["model"],
            "end": side_summary(errors["right"]["end"], rate),
            "start": side_summary(errors["right"]["start"], rate),
        },
        "errors_frames": errors,
    }


def envelope_junction_directories(
    audio_directory: Path,
    left_directory: Path,
    right_directory: Path,
    minimum_pause_frames: int,
    select: str = "both",
    speech_ratio_percent: int = ENVELOPE_SPEECH_RATIO_PERCENT,
) -> dict[str, Any]:
    left = manifests_by_sample(left_directory, "left")
    right = manifests_by_sample(right_directory, "right")
    if left.keys() != right.keys():
        raise BenchmarkError("left and right directories contain different samples")
    details = []
    pooled: dict[str, dict[str, list[int]]] = {
        side: {"end": [], "start": []} for side in ("left", "right")
    }
    for sample_id in sorted(left):
        source_rate = left[sample_id]["source_rate"]
        rate = (source_rate["num"], source_rate["den"])
        _, powers = envelope_window_powers(audio_directory / f"{sample_id}.wav")
        frames = left[sample_id]["processing"]["audio_duration_frames"]
        detail = envelope_junctions(
            left[sample_id],
            right[sample_id],
            voiced_frames(powers, rate, frames, speech_ratio_percent),
            minimum_pause_frames,
            select=select,
        )
        for side in pooled:
            for edge in ("end", "start"):
                pooled[side][edge].extend(detail["errors_frames"][side][edge])
        details.append(detail)
    rates = {
        (value["source_rate"]["num"], value["source_rate"]["den"])
        for value in left.values()
    }
    if len(rates) != 1:
        raise BenchmarkError("pooled junctions need one frame rate")
    rate = rates.pop()

    def both_within_one(side: str) -> float | None:
        pairs = list(zip(pooled[side]["end"], pooled[side]["start"]))
        if not pairs:
            return None
        hits = sum(1 for end, start in pairs if abs(end) <= 1 and abs(start) <= 1)
        return 100 * hits / len(pairs)

    # Junction by junction, which aligner sits closer to the edge (ties
    # apart): a mean can hide that one aligner wins most junctions and loses
    # a few badly.
    closer = {
        edge: {
            "left": sum(
                1
                for a, c in zip(pooled["left"][edge], pooled["right"][edge])
                if abs(a) < abs(c)
            ),
            "right": sum(
                1
                for a, c in zip(pooled["left"][edge], pooled["right"][edge])
                if abs(c) < abs(a)
            ),
        }
        for edge in ("end", "start")
    }
    return {
        "schema_version": 1,
        "status": "envelope_proxy_not_accuracy",
        "select": select,
        "speech_ratio_percent": speech_ratio_percent,
        "minimum_pause_frames": minimum_pause_frames,
        "samples": len(details),
        "pause_junctions": sum(d["pause_junctions"] for d in details),
        "ambiguous_junctions": sum(d["ambiguous_junctions"] for d in details),
        "left": {
            "method": details[0]["left"]["method"],
            "end": side_summary(pooled["left"]["end"], rate),
            "start": side_summary(pooled["left"]["start"], rate),
            "both_within_one_frame_percent": both_within_one("left"),
        },
        "right": {
            "method": details[0]["right"]["method"],
            "end": side_summary(pooled["right"]["end"], rate),
            "start": side_summary(pooled["right"]["start"], rate),
            "both_within_one_frame_percent": both_within_one("right"),
        },
        "closer": closer,
        "details": details,
    }


# Junction gold: the boundaries a cut is placed on, annotated blind. The full
# word gold of the protocol above (every word, two annotators) was never
# produced; what the engine cuts on is the junction around a pause, so this
# asks for those only. The kit names each junction by its words and never by
# a time: the annotator finds it by ear, in an editor, and places two markers,
# so no aligner's frame can leak into the truth. Candidates are the union of
# every aligner's gaps and the envelope's pauses, so the selection does not
# favour one of them either.
JUNCTION_MARKER = re.compile(r"J0*(\d+)\s*[-_ ]?\s*([FD])\b", re.IGNORECASE)
TIMECODE = re.compile(r"\b(\d{2}):(\d{2}):(\d{2})[:;](\d{2})\b")
LOC_LINE = re.compile(r"^\*\s*LOC:\s*(\d{2}:\d{2}:\d{2}[:;]\d{2})\s+\S+\s+(.*)$")
# A word heard but absent from the sheet: « M euh », « M manque ».
UNWRITTEN_MARKER = re.compile(r"M\s+(.+)", re.IGNORECASE)


def junction_candidates(
    base: dict[str, Any],
    others: list[dict[str, Any]],
    voiced: list[bool],
    minimum_pause_frames: int,
    search_frames: int = 10,
) -> list[tuple[int, int]]:
    words = base["words"]
    maps = [dict(matched_word_indices(words, other["words"])) for other in others]
    result = []
    for index in range(len(words) - 1):
        gaps = [words[index + 1]["frame_start"] - words[index]["frame_end"]]
        for other, mapping in zip(others, maps):
            if index in mapping and mapping.get(index + 1) == mapping[index] + 1:
                ow = other["words"]
                gaps.append(
                    ow[mapping[index + 1]]["frame_start"]
                    - ow[mapping[index]]["frame_end"]
                )
        pauses = [
            run
            for run in pause_runs(
                voiced,
                words[index]["frame_end"] - search_frames,
                words[index + 1]["frame_start"] + search_frames,
                minimum_pause_frames,
            )
            if run[0] > words[index]["frame_start"]
            and run[1] < words[index + 1]["frame_end"]
        ]
        if max(gaps) >= minimum_pause_frames or pauses:
            result.append((index, index + 1))
    return result


def junction_sheet(kit: dict[str, Any]) -> str:
    marks = {junction["before_index"]: junction["id"] for junction in kit["junctions"]}
    tokens = []
    for index, text in enumerate(kit["reference_words"]):
        tokens.append(text)
        if index in marks:
            tokens.append(f"[{marks[index]}]")
    lines, line = [], ""
    for token in tokens:
        if line and len(line) + 1 + len(token) > 78:
            lines.append(line)
            line = token
        else:
            line = f"{line} {token}" if line else token
    if line:
        lines.append(line)
    header = [
        f"Échantillon {kit['sample_id']} : {len(kit['junctions'])} jointures, "
        f"{2 * len(kit['junctions'])} marqueurs.",
        "",
        "Pour chaque [Jnn] : un marqueur « Jnn F » sur la dernière image où le",
        "mot d'avant s'entend encore, un marqueur « Jnn D » sur la première image",
        "où le mot d'après s'entend. Sans pause audible, D suit F d'une image.",
        "Les respirations ne sont pas des mots ; un « euh » prononcé en est un.",
        "",
        "Un mot entendu qui manque à la feuille (« euh », répétition, faux",
        "départ, son étiré) : un marqueur « M mot » sur sa première image,",
        "étiré sur toute sa durée s'il en a une (« M euh », « M manque »).",
        "",
    ]
    return "\n".join(header + lines) + "\n"


def junction_kit(
    base: dict[str, Any],
    others: list[dict[str, Any]],
    voiced: list[bool],
    minimum_pause_frames: int,
) -> dict[str, Any]:
    candidates = junction_candidates(base, others, voiced, minimum_pause_frames)
    words = base["words"]
    return {
        "schema_version": 1,
        "kind": "junction_kit",
        "sample_id": base["sample_id"],
        "media_id": base["media_id"],
        "source_rate": base["source_rate"],
        "audio_duration_frames": base["processing"]["audio_duration_frames"],
        "minimum_pause_frames": minimum_pause_frames,
        "selection": "aligner_gap_or_envelope_pause",
        "selected_from": sorted(
            {value["alignment"]["method"] for value in [base] + others}
        ),
        "reference_words": [word["text"] for word in words],
        "junctions": [
            {
                "id": f"J{number:03d}",
                "before_index": before,
                "after_index": after,
                "before": words[before]["text"],
                "after": words[after]["text"],
            }
            for number, (before, after) in enumerate(candidates, start=1)
        ],
    }


def junction_kit_directory(
    audio_directory: Path,
    base_directory: Path,
    other_directories: list[Path],
    output_directory: Path,
    minimum_pause_frames: int,
) -> dict[str, Any]:
    base = manifests_by_sample(base_directory, "base")
    others = [manifests_by_sample(path, "other") for path in other_directories]
    written = {}
    for sample_id in sorted(base):
        value = base[sample_id]
        rate = (value["source_rate"]["num"], value["source_rate"]["den"])
        _, powers = envelope_window_powers(audio_directory / f"{sample_id}.wav")
        voiced = voiced_frames(
            powers, rate, value["processing"]["audio_duration_frames"]
        )
        kit = junction_kit(
            value,
            [other[sample_id] for other in others if sample_id in other],
            voiced,
            minimum_pause_frames,
        )
        write_json(output_directory / f"{sample_id}.kit.json", kit)
        write_text(output_directory / f"{sample_id}.feuille.txt", junction_sheet(kit))
        written[sample_id] = len(kit["junctions"])
    return {"junctions": written, "markers": 2 * sum(written.values())}


def timecode_frames(match: re.Match[str], fps: int) -> int:
    hours, minutes, seconds, frames = (int(group) for group in match.groups())
    if frames >= fps:
        raise BenchmarkError(f"timecode {match.group(0)} has no frame at {fps} fps")
    return ((hours * 60 + minutes) * 60 + seconds) * fps + frames


def edl_marker_list(
    text: str, fps: int, timeline_start: int
) -> list[tuple[str, int, int]]:
    """Every marker of a timeline-marker EDL: name, frame, duration.

    Both marker lines Resolve writes are read: ` |C:… |M:name |D:n` under an
    event, which takes the event's record in, and `* LOC: tc COLOUR name`.
    The duration is the event's record out less its record in, one frame
    when nothing gives one.
    """
    result = []
    event: tuple[int, int] | None = None
    for line in text.splitlines():
        codes = list(TIMECODE.finditer(line))
        if len(codes) >= 4:
            record_in = timecode_frames(codes[2], fps) - timeline_start
            record_out = timecode_frames(codes[3], fps) - timeline_start
            event = (record_in, max(1, record_out - record_in))
            continue
        location = LOC_LINE.match(line)
        if location:
            code = TIMECODE.search(location.group(1))
            frame = timecode_frames(code, fps) - timeline_start
            duration = event[1] if event is not None and event[0] == frame else 1
            name = location.group(2).strip()
        elif "|M:" in line and event is not None:
            frame, duration = event
            name = line.split("|M:", 1)[1].split("|", 1)[0].strip()
        else:
            continue
        if frame < 0:
            raise BenchmarkError(f"marker « {name} » is before the timeline start")
        result.append((name, frame, duration))
        event = None
    return result


def edl_markers(text: str, fps: int, timeline_start: int) -> dict[str, dict[str, int]]:
    """Read « Jnn F » / « Jnn D » markers from a timeline-marker EDL."""
    markers: dict[str, dict[str, int]] = defaultdict(dict)
    for name, frame, _ in edl_marker_list(text, fps, timeline_start):
        match = JUNCTION_MARKER.fullmatch(name)
        if not match:
            continue
        junction = f"J{int(match.group(1)):03d}"
        edge = match.group(2).upper()
        if edge in markers[junction]:
            raise BenchmarkError(f"marker {junction} {edge} appears twice")
        markers[junction][edge] = frame
    return dict(markers)


def edl_unwritten_words(
    text: str, fps: int, timeline_start: int
) -> list[dict[str, Any]]:
    """Read « M mot » markers: words heard that the sheet does not have."""
    words = []
    for name, frame, duration in edl_marker_list(text, fps, timeline_start):
        match = UNWRITTEN_MARKER.fullmatch(name)
        if match and not JUNCTION_MARKER.fullmatch(name):
            words.append(
                {
                    "text": match.group(1).strip(),
                    "frame_start": frame,
                    "frame_end": frame + duration,
                }
            )
    return sorted(words, key=lambda word: (word["frame_start"], word["text"]))


def junction_gold(
    kit: dict[str, Any],
    markers: dict[str, dict[str, int]],
    annotator: str,
    unwritten: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if kit.get("kind") != "junction_kit":
        raise BenchmarkError("not a junction kit")
    known = {junction["id"] for junction in kit["junctions"]}
    unknown = sorted(set(markers) - known)
    if unknown:
        raise BenchmarkError(f"markers name junctions the kit does not have: {unknown}")
    annotated, missing = [], []
    for junction in kit["junctions"]:
        edges = markers.get(junction["id"], {})
        if "F" not in edges or "D" not in edges:
            missing.append(junction["id"])
            continue
        end = edges["F"] + 1
        if edges["D"] < end:
            raise BenchmarkError(
                f"{junction['id']}: D is before the frame after F"
            )
        annotated.append(
            {
                "id": junction["id"],
                "before_index": junction["before_index"],
                "after_index": junction["after_index"],
                "end_frame": end,
                "start_frame": edges["D"],
            }
        )
    kit_bytes = json.dumps(kit, ensure_ascii=False, sort_keys=True).encode("utf-8")
    return {
        "schema_version": 1,
        "kind": "junction_gold",
        "sample_id": kit["sample_id"],
        "media_id": kit["media_id"],
        "source_rate": kit["source_rate"],
        "annotator": annotator,
        "kit_sha256": hashlib.sha256(kit_bytes).hexdigest(),
        "reference_words": kit["reference_words"],
        "junctions": annotated,
        "missing": missing,
        "unwritten_words": unwritten or [],
    }


def score_junctions(gold: dict[str, Any], predicted: dict[str, Any]) -> dict[str, Any]:
    if gold.get("kind") != "junction_gold":
        raise BenchmarkError("not a junction gold")
    validate_manifest(predicted)
    if gold["source_rate"] != predicted["source_rate"]:
        raise BenchmarkError("gold and prediction do not share a frame rate")
    rate = (gold["source_rate"]["num"], gold["source_rate"]["den"])
    reference = [{"text": text} for text in gold["reference_words"]]
    mapping = dict(matched_word_indices(reference, predicted["words"]))
    words = predicted["words"]
    ends, starts, uncovered = [], [], []
    for junction in gold["junctions"]:
        before = mapping.get(junction["before_index"])
        after = mapping.get(junction["after_index"])
        if before is None or after is None:
            uncovered.append(junction["id"])
            continue
        ends.append(words[before]["frame_end"] - junction["end_frame"])
        starts.append(words[after]["frame_start"] - junction["start_frame"])
    both = sum(
        1 for end, start in zip(ends, starts) if abs(end) <= 1 and abs(start) <= 1
    )
    return {
        "sample_id": gold["sample_id"],
        "method": predicted["alignment"]["method"],
        "model": predicted["alignment"]["model"],
        "junctions": len(gold["junctions"]),
        "scored": len(ends),
        "uncovered": uncovered,
        "end": side_summary(ends, rate),
        "start": side_summary(starts, rate),
        "both_within_one_frame_percent": 100 * both / len(ends) if ends else None,
        "errors_frames": {"end": ends, "start": starts},
    }


def score_junction_directories(
    gold_directory: Path, predictions_directory: Path
) -> dict[str, Any]:
    golds = {}
    for path in sorted(gold_directory.glob("*.json")):
        value = load_json(path)
        if value.get("kind") == "junction_gold":
            golds[value["sample_id"]] = value
    if not golds:
        raise BenchmarkError("gold directory contains no junction gold")
    predicted = manifests_by_sample(predictions_directory, "predictions")
    missing = sorted(set(golds) - set(predicted))
    if missing:
        raise BenchmarkError(f"no prediction for {missing}")
    details = [
        score_junctions(golds[sample_id], predicted[sample_id])
        for sample_id in sorted(golds)
    ]
    rates = {
        (gold["source_rate"]["num"], gold["source_rate"]["den"])
        for gold in golds.values()
    }
    if len(rates) != 1:
        raise BenchmarkError("pooled junctions need one frame rate")
    rate = rates.pop()
    ends = [value for detail in details for value in detail["errors_frames"]["end"]]
    starts = [value for detail in details for value in detail["errors_frames"]["start"]]
    both = sum(
        1 for end, start in zip(ends, starts) if abs(end) <= 1 and abs(start) <= 1
    )
    return {
        "schema_version": 1,
        "status": "junction_accuracy",
        "method": details[0]["method"],
        "junctions": sum(detail["junctions"] for detail in details),
        "scored": len(ends),
        "end": side_summary(ends, rate),
        "start": side_summary(starts, rate),
        "both_within_one_frame_percent": 100 * both / len(ends) if ends else None,
        "details": details,
    }


# Words heard and not written. The annotator marks them (« M euh ») while
# placing the junction markers; two questions are then asked of any
# manifest. Does a transcript write them: a word its text has beyond the
# reference text, at the place of a marked word and spelled the same? And
# does an alignment of the reference text leave them as holes: voice outside
# every aligned word, which the engine's « voice outside words » check
# (VoiceOutsideWords, A18) can find? An aligner that stretches the
# neighbours over a missing word hides it from that check.
#
# Holes are read on the aligner's own frames by default, or on the frames the
# engine gives each word (`engine=True`): its entry and exit rules widen a
# word to the voice (ResolveWordBoundaries, src/media/WordBoundaries.cc), and
# VoiceOutsideWords counts those frames as the word's. A first version of
# this check approximated the rules by a fixed air around every word; the
# exit that follows the voice is what swallows a missing word said in one
# breath with its neighbour, and a fixed air does not model it.
UNWRITTEN_TOLERANCE_FRAMES = 3

# WordBoundaryRules defaults (src/media/WordBoundaries.h).
ENGINE_ENTRY_SEARCH_FRAMES = 10
ENGINE_ATTACK_SEARCH_FRAMES = 25
ENGINE_ENTRY_AIR_FRAMES = 2
ENGINE_EXIT_AIR_FRAMES = 3
ENGINE_DROP_SEARCH_FRAMES = 25


def hole_minimum_frames(rate: tuple[int, int]) -> int:
    # At least 160 ms, rounded up to whole frames, as the engine scans
    # (TimelineWords.cc, McpSpeechTools.cc).
    return max(1, -(-16 * rate[0] // (100 * rate[1])))


def engine_word_spans(
    words: list[dict[str, Any]], voiced: list[bool]
) -> list[tuple[int, int]]:
    """Each word's frames as the engine's voice-outside-words check reads them.

    A port of ResolveWordBoundaries with the default rules: from the earlier
    of the aligner's start and the entry to the later of its end and the exit.
    """

    def is_voiced(frame: int) -> bool:
        return 0 <= frame < len(voiced) and voiced[frame]

    spans = []
    for index, word in enumerate(words):
        start, end = word["frame_start"], word["frame_end"]
        entry, exit_frame = start, end
        if not any(is_voiced(frame) for frame in range(start, end)):
            spans.append((start, end))
            continue
        if not is_voiced(start):
            ahead = range(start + 1, min(end, start + ENGINE_ATTACK_SEARCH_FRAMES + 1))
            attack = next((frame for frame in ahead if is_voiced(frame)), None)
            if attack is not None:
                entry = max(start, attack - ENGINE_ENTRY_AIR_FRAMES)
        else:
            lowest = max(0, start - ENGINE_ENTRY_SEARCH_FRAMES)
            for frame in range(start - 1, lowest - 1, -1):
                if not is_voiced(frame):
                    entry = frame
                    if frame - 1 >= 0 and not is_voiced(frame - 1):
                        entry = frame - 1
                    break
        next_start = (
            words[index + 1]["frame_start"] if index + 1 < len(words) else math.inf
        )
        if end > start and is_voiced(end - 1):
            drop = None
            for frame in range(end, end + ENGINE_DROP_SEARCH_FRAMES + 1):
                if frame >= next_start:
                    break
                if not is_voiced(frame):
                    drop = frame
                    break
            if drop is not None:
                exit_frame = drop
            elif next_start <= end + ENGINE_DROP_SEARCH_FRAMES:
                exit_frame = max(end, next_start)
        else:
            last_voiced = next(
                (frame for frame in range(end - 1, start - 1, -1) if is_voiced(frame)),
                None,
            )
            if last_voiced is not None:
                exit_frame = min(end, last_voiced + 1 + ENGINE_EXIT_AIR_FRAMES)
        spans.append((min(start, entry), max(end, exit_frame)))
    return spans


def voice_outside_words(
    words: list[dict[str, Any]],
    voiced: list[bool],
    minimum_frames: int,
    engine: bool = False,
) -> list[tuple[int, int]]:
    covered = [False] * len(voiced)
    spans = (
        engine_word_spans(words, voiced)
        if engine
        else [(word["frame_start"], word["frame_end"]) for word in words]
    )
    for first, last in spans:
        for frame in range(max(0, first), min(len(voiced), last)):
            covered[frame] = True
    runs, frame = [], 0
    while frame < len(voiced):
        if voiced[frame] and not covered[frame]:
            start = frame
            while frame < len(voiced) and voiced[frame] and not covered[frame]:
                frame += 1
            if frame - start >= minimum_frames:
                runs.append((start, frame))
        else:
            frame += 1
    return runs


def overlaps(
    first: int, end: int, word: dict[str, Any], tolerance: int = 0
) -> bool:
    return (
        first < word["frame_end"] + tolerance
        and word["frame_start"] - tolerance < end
    )


def score_unwritten(
    gold: dict[str, Any],
    predicted: dict[str, Any],
    voiced: list[bool] | None,
    engine: bool = False,
) -> dict[str, Any]:
    if gold.get("kind") != "junction_gold":
        raise BenchmarkError("not a junction gold")
    validate_manifest(predicted)
    if gold["source_rate"] != predicted["source_rate"]:
        raise BenchmarkError("gold and prediction do not share a frame rate")
    unwritten = gold.get("unwritten_words", [])
    words = predicted["words"]
    reference = [{"text": text} for text in gold["reference_words"]]
    in_reference = {index for _, index in matched_word_indices(reference, words)}
    extra = [word for index, word in enumerate(words) if index not in in_reference]
    tolerance = UNWRITTEN_TOLERANCE_FRAMES
    written, used = [], set()
    for word in unwritten:
        key = folded_word(word["text"])
        hit = next(
            (
                index
                for index, candidate in enumerate(extra)
                if index not in used
                and folded_word(candidate["text"]) == key
                and overlaps(
                    word["frame_start"], word["frame_end"], candidate, tolerance
                )
            ),
            None,
        )
        if hit is not None:
            used.add(hit)
            written.append(word["text"])
    result: dict[str, Any] = {
        "sample_id": gold["sample_id"],
        "method": predicted["alignment"]["method"],
        "model": predicted["alignment"]["model"],
        "unwritten_words": len(unwritten),
        "written": len(written),
        "written_words": written,
        "extra_words": len(extra),
        "extra_not_marked": len(extra) - len(used),
    }
    if voiced is not None:
        rate = (gold["source_rate"]["num"], gold["source_rate"]["den"])
        holes = voice_outside_words(
            words, voiced, hole_minimum_frames(rate), engine
        )
        # The tolerance covers a repetition: of « manque manque », which of
        # the two the transcript lacks is the annotator's guess.
        exposed = [
            word["text"]
            for word in unwritten
            if any(overlaps(start, end, word, tolerance) for start, end in holes)
        ]
        result["word_frames"] = "engine" if engine else "aligner"
        result["holes"] = len(holes)
        result["exposed"] = len(exposed)
        result["exposed_words"] = exposed
        result["holes_on_no_marked_word"] = sum(
            1
            for start, end in holes
            if not any(overlaps(start, end, word, tolerance) for word in unwritten)
        )
    return result


def score_unwritten_directories(
    gold_directory: Path,
    predictions_directory: Path,
    audio_directory: Path | None,
    engine: bool = False,
) -> dict[str, Any]:
    golds = {}
    for path in sorted(gold_directory.glob("*.json")):
        value = load_json(path)
        if value.get("kind") == "junction_gold":
            golds[value["sample_id"]] = value
    if not golds:
        raise BenchmarkError("gold directory contains no junction gold")
    predicted = manifests_by_sample(predictions_directory, "predictions")
    missing = sorted(set(golds) - set(predicted))
    if missing:
        raise BenchmarkError(f"no prediction for {missing}")
    details = []
    for sample_id in sorted(golds):
        voiced = None
        if audio_directory is not None:
            value = predicted[sample_id]
            rate = (value["source_rate"]["num"], value["source_rate"]["den"])
            _, powers = envelope_window_powers(audio_directory / f"{sample_id}.wav")
            voiced = voiced_frames(
                powers, rate, value["processing"]["audio_duration_frames"]
            )
        details.append(
            score_unwritten(golds[sample_id], predicted[sample_id], voiced, engine)
        )
    total = sum(detail["unwritten_words"] for detail in details)
    result: dict[str, Any] = {
        "schema_version": 1,
        "status": "unwritten_words",
        "method": details[0]["method"],
        "unwritten_words": total,
        "written": sum(detail["written"] for detail in details),
        "extra_not_marked": sum(detail["extra_not_marked"] for detail in details),
        "details": details,
    }
    if audio_directory is not None:
        result["exposed"] = sum(detail["exposed"] for detail in details)
        result["holes_on_no_marked_word"] = sum(
            detail["holes_on_no_marked_word"] for detail in details
        )
    return result


def score_directories(
    gold_directory: Path, prediction_directory: Path
) -> dict[str, Any]:
    gold_by_sample = {}
    for value in discover_manifests(gold_directory):
        sample_id = value["sample_id"]
        if sample_id in gold_by_sample:
            raise BenchmarkError(f"duplicate gold manifest for '{sample_id}'")
        gold_by_sample[sample_id] = value
    if not gold_by_sample:
        raise BenchmarkError("gold directory contains no valid manifests")
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    reference_hashes: dict[str, set[str]] = defaultdict(set)
    detailed = []
    seen = set()
    for predicted in discover_manifests(prediction_directory):
        sample_id = predicted["sample_id"]
        if sample_id not in gold_by_sample:
            raise BenchmarkError(f"prediction has no gold manifest: '{sample_id}'")
        key = (
            predicted["alignment"]["method"],
            predicted["alignment"]["model"],
            sample_id,
        )
        if key in seen:
            raise BenchmarkError(f"duplicate prediction for {key}")
        seen.add(key)
        reference_hash = predicted["alignment"]["reference_text_sha256"]
        if reference_hash is None:
            raise BenchmarkError(f"prediction has no reference text hash: {key}")
        reference_hashes[sample_id].add(reference_hash)
        score = score_pair(gold_by_sample[sample_id], predicted)
        detailed.append(public_score(score))
        grouped[key[:2]].append(score)
    for sample_id, hashes in reference_hashes.items():
        if len(hashes) != 1:
            raise BenchmarkError(
                f"predictions for '{sample_id}' did not use the same reference text"
            )
    aggregate = [aggregate_scores(group) for _, group in sorted(grouped.items())]
    return {"schema_version": 1, "aggregate": aggregate, "samples": detailed}


def inventory_cutmachine(directories: list[Path]) -> dict[str, Any]:
    files = []
    models: Counter[str] = Counter()
    aligned = Counter()
    safety = Counter()
    word_count = 0
    invalid = []
    for directory in directories:
        for path in sorted(directory.rglob("*.json")):
            # Finder resource forks are AppleDouble metadata, not transcripts.
            if path.name.startswith("._"):
                continue
            try:
                value = load_json(path)
                if not isinstance(value.get("words"), list):
                    raise BenchmarkError("missing words array")
            except BenchmarkError as exc:
                invalid.append(str(exc))
                continue
            files.append(str(path))
            word_count += len(value["words"])
            models[str(value.get("whisper_model", "unknown"))] += 1
            alignment_key = (
                "speech_aligned" if value.get("speech_aligned") else "not_aligned"
            )
            aligned[alignment_key] += 1
            for field in (
                "speech_assessed",
                "likely_hallucinated",
                "likely_incomplete",
                "known_hallucination_phrase",
            ):
                if value.get(field):
                    safety[field] += 1
    return {
        "schema_version": 1,
        "directories": [str(path) for path in directories],
        "transcript_files": len(files),
        "words": word_count,
        "models": dict(sorted(models.items())),
        "alignment": dict(sorted(aligned.items())),
        "safety_flags": dict(sorted(safety.items())),
        "invalid_files": invalid,
    }


def common_normalize_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sample-id", required=True)
    parser.add_argument("--media-id", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--wall-time-ms", type=int, required=True)
    parser.add_argument("--audio-duration-frames", type=int, required=True)
    parser.add_argument("--speaker-id")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate")
    validate.add_argument("manifests", nargs="+", type=Path)

    current = subparsers.add_parser("normalize-cutmachine")
    common_normalize_arguments(current)

    mfa = subparsers.add_parser("normalize-mfa")
    common_normalize_arguments(mfa)
    mfa.add_argument("--source-rate", required=True)
    mfa.add_argument("--model", required=True)
    mfa.add_argument("--reference-text", type=Path, required=True)
    mfa.add_argument("--tier")

    nemo = subparsers.add_parser("normalize-nemo")
    common_normalize_arguments(nemo)
    nemo.add_argument("--source-rate", required=True)
    nemo.add_argument("--model", required=True)
    nemo.add_argument("--reference-text", type=Path, required=True)

    qwen = subparsers.add_parser("normalize-qwen")
    common_normalize_arguments(qwen)
    qwen.add_argument("--source-rate", required=True)
    qwen.add_argument("--model", required=True)
    qwen.add_argument("--reference-text", type=Path, required=True)

    ensemble = subparsers.add_parser("ensemble")
    ensemble.add_argument("--inputs", nargs="+", type=Path, required=True)
    ensemble.add_argument("--output", type=Path, required=True)
    ensemble.add_argument("--srt-output", type=Path)
    ensemble.add_argument("--wall-time-ms", type=int, required=True)

    score = subparsers.add_parser("score")
    score.add_argument("--gold-dir", type=Path, required=True)
    score.add_argument("--predictions-dir", type=Path, required=True)
    score.add_argument("--output", type=Path)

    resolve_score = subparsers.add_parser("score-resolve")
    resolve_score.add_argument("--gold", type=Path, required=True)
    resolve_score.add_argument("--input", type=Path, required=True)
    resolve_score.add_argument("--output", type=Path)

    agreement = subparsers.add_parser("agreement")
    agreement.add_argument("--left", type=Path, required=True)
    agreement.add_argument("--right", type=Path, required=True)
    agreement.add_argument("--output", type=Path)

    agreement_set = subparsers.add_parser("agreement-set")
    agreement_set.add_argument("--left-dir", type=Path, required=True)
    agreement_set.add_argument("--right-dir", type=Path, required=True)
    agreement_set.add_argument("--output", type=Path)

    junctions = subparsers.add_parser("envelope-junctions")
    junctions.add_argument("--audio-dir", type=Path, required=True)
    junctions.add_argument("--left-dir", type=Path, required=True)
    junctions.add_argument("--right-dir", type=Path, required=True)
    junctions.add_argument("--minimum-pause-frames", type=int, default=4)
    junctions.add_argument(
        "--select", choices=["both", "left", "right"], default="both"
    )
    junctions.add_argument(
        "--speech-ratio-percent", type=int, default=ENVELOPE_SPEECH_RATIO_PERCENT
    )
    junctions.add_argument("--output", type=Path)

    kit = subparsers.add_parser("junction-kit")
    kit.add_argument("--audio-dir", type=Path, required=True)
    kit.add_argument("--base-dir", type=Path, required=True)
    kit.add_argument("--other-dir", type=Path, action="append", default=[])
    kit.add_argument("--output-dir", type=Path, required=True)
    kit.add_argument("--minimum-pause-frames", type=int, default=5)

    gold = subparsers.add_parser("junction-gold")
    gold.add_argument("--kit", type=Path, required=True)
    gold.add_argument("--edl", type=Path, required=True)
    gold.add_argument("--fps", type=int, default=25)
    gold.add_argument("--timeline-start", default="01:00:00:00")
    gold.add_argument("--annotator", required=True)
    gold.add_argument("--output", type=Path, required=True)

    score_junction = subparsers.add_parser("score-junctions")
    score_junction.add_argument("--gold-dir", type=Path, required=True)
    score_junction.add_argument("--predictions-dir", type=Path, required=True)
    score_junction.add_argument("--output", type=Path)

    unwritten = subparsers.add_parser("score-unwritten")
    unwritten.add_argument("--gold-dir", type=Path, required=True)
    unwritten.add_argument("--predictions-dir", type=Path, required=True)
    unwritten.add_argument("--audio-dir", type=Path)
    unwritten.add_argument("--engine-boundaries", action="store_true")
    unwritten.add_argument("--output", type=Path)

    inventory = subparsers.add_parser("inventory-cutmachine")
    inventory.add_argument("directories", nargs="+", type=Path)
    inventory.add_argument("--output", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(argv or sys.argv[1:])
    try:
        if args.command == "validate":
            for path in args.manifests:
                validate_manifest(load_json(path))
            result: Any = {"valid": len(args.manifests)}
        elif args.command == "normalize-cutmachine":
            result = cutmachine_manifest(args)
            write_json(args.output, result)
        elif args.command in ("normalize-mfa", "normalize-nemo", "normalize-qwen"):
            result = alignment_manifest(args)
            write_json(args.output, result)
        elif args.command == "ensemble":
            result = ensemble_manifests(
                [load_json(path) for path in args.inputs], args.wall_time_ms
            )
            srt = srt_for_manifest(result) if args.srt_output else None
            write_json(args.output, result)
            if args.srt_output and srt is not None:
                write_text(args.srt_output, srt)
        elif args.command == "score":
            result = score_directories(args.gold_dir, args.predictions_dir)
            if args.output:
                write_json(args.output, result)
        elif args.command == "score-resolve":
            result = score_resolve_captions(
                load_json(args.gold), load_json(args.input)
            )
            if args.output:
                write_json(args.output, result)
        elif args.command == "agreement":
            result = agreement_pair(load_json(args.left), load_json(args.right))
            if args.output:
                write_json(args.output, result)
        elif args.command == "agreement-set":
            result = agreement_directories(args.left_dir, args.right_dir)
            if args.output:
                write_json(args.output, result)
        elif args.command == "envelope-junctions":
            result = envelope_junction_directories(
                args.audio_dir, args.left_dir, args.right_dir,
                args.minimum_pause_frames, args.select, args.speech_ratio_percent,
            )
            if args.output:
                write_json(args.output, result)
        elif args.command == "junction-kit":
            result = junction_kit_directory(
                args.audio_dir, args.base_dir, args.other_dir, args.output_dir,
                args.minimum_pause_frames,
            )
        elif args.command == "junction-gold":
            start = TIMECODE.fullmatch(args.timeline_start)
            if not start:
                raise BenchmarkError("--timeline-start is not HH:MM:SS:FF")
            try:
                edl = args.edl.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise BenchmarkError(f"{args.edl}: {exc}") from exc
            first = timecode_frames(start, args.fps)
            result = junction_gold(
                load_json(args.kit),
                edl_markers(edl, args.fps, first),
                args.annotator,
                edl_unwritten_words(edl, args.fps, first),
            )
            write_json(args.output, result)
        elif args.command == "score-junctions":
            result = score_junction_directories(args.gold_dir, args.predictions_dir)
            if args.output:
                write_json(args.output, result)
        elif args.command == "score-unwritten":
            result = score_unwritten_directories(
                args.gold_dir,
                args.predictions_dir,
                args.audio_dir,
                args.engine_boundaries,
            )
            if args.output:
                write_json(args.output, result)
        else:
            result = inventory_cutmachine(args.directories)
            if args.output:
                write_json(args.output, result)
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    except BenchmarkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
