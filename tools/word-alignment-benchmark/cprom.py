#!/usr/bin/env python3
"""Measure word aligners against C-PROM's hand-verified French word tier.

C-PROM (Avanzi, Simon, Goldman & Auchlin, 2010; CC BY-NC-SA) is 70 minutes
of French in seven speaking styles, aligned by EasyAlign and checked by hand,
hesitations written. It is the only hand-checked French word alignment the
bench has that no aligner under test produced. Being NC, it stays a local
test set: never committed, never redistributed.

`prepare` cuts every recording into chunks of at most 60 s, in the middle of
a gold pause of at least 300 ms (Qwen takes five minutes at most, MFA aligns
a whole file as one utterance), and writes each aligner's input from the
gold words: an elided clitic joined to the next word as a transcript writes
it (« c' » + « est »), NeMo's text through the engine's own normalization
(`spoken` in scripts/mfa_align.py, formerly nemo_align.py). `score` pairs each aligner's raw words
with the gold by longest common subsequence and reports milliseconds.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import random
import shutil
import statistics
import subprocess
import sys
from decimal import Decimal
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


BENCHMARK = load("word_alignment_benchmark", HERE / "benchmark.py")
NEMO_ALIGN = load("mfa_align", ROOT / "scripts" / "mfa_align.py")
BenchmarkError = BENCHMARK.BenchmarkError

MAX_CHUNK_SECONDS = 60.0
CUT_PAUSE_SECONDS = 0.3
# Pauses, a doubled pause, an empty interval, external noise.
NOT_WORDS = {"_", "__", "", "%"}
METHODS = ("nemo", "qwen", "mfa")
FILLERS = {"euh", "ben", "mh", "hein", "eh", "bah"}
FORMAL_STYLES = {"lec", "pol", "jpa", "cnf"}
# An edge next to a gold pause at least this long is one an edit cuts at.
CUT_EDGE_PAUSE_SECONDS = 0.15
TOLERANCE_MS = 40  # one frame at 25 fps


def gold_words(
    entries: list[tuple[float, float | None, str]],
) -> list[dict[str, Any]]:
    """The word tier as a transcript would write it, pauses kept apart.

    An interval holding several words (« il|y », overlapping speech) keeps
    its words in the text but is not scored: no aligner can be compared
    with one boundary for several words.
    """
    items: list[dict[str, Any]] = []
    for start, end, label in entries:
        text = "".join(c for c in label if c.isprintable()).strip()
        if text in NOT_WORDS:
            items.append({"pause": True, "start": start, "end": end})
            continue
        text = text.replace("|", " ")
        previous = items[-1] if items else None
        if (
            previous
            and not previous.get("pause")
            and previous["text"].endswith("'")
            and " " not in previous["text"]
            and " " not in text
        ):
            previous["text"] += text
            previous["end"] = end
            continue
        items.append(
            {"text": text, "start": start, "end": end, "scored": " " not in text}
        )
    return items


def chunks_of(
    items: list[dict[str, Any]], duration: float
) -> list[tuple[float, float, list[dict[str, Any]]]]:
    """Cut at the first long pause past half the longest chunk."""
    chunks, current, chunk_start = [], [], 0.0
    for item in items:
        if item.get("pause"):
            middle = (item["start"] + item["end"]) / 2
            if (
                current
                and item["end"] - item["start"] >= CUT_PAUSE_SECONDS
                and middle - chunk_start >= MAX_CHUNK_SECONDS / 2
            ):
                chunks.append((chunk_start, middle, current))
                current, chunk_start = [], middle
            continue
        if item["end"] - chunk_start > MAX_CHUNK_SECONDS and current:
            raise BenchmarkError(f"no pause to cut before {item['start']:.2f} s")
        current.append(item)
    if current:
        chunks.append((chunk_start, duration, current))
    return chunks



def prepare(source: Path, output: Path) -> dict[str, Any]:
    for directory in ("chunks", "mfa-corpus"):
        (output / directory).mkdir(parents=True, exist_ok=True)
    gold: dict[str, Any] = {}
    lines: dict[str, list[dict[str, Any]]] = {"nemo": [], "qwen": []}
    for grid in sorted(source.glob("*.TextGrid")):
        words = BENCHMARK.praat_binary_tiers(grid)["words"]
        duration = words[-1][1]
        cut = chunks_of(gold_words(words), duration)
        for index, (start, end, items) in enumerate(cut):
            chunk_id = f"{grid.stem}-{index:02d}"
            wav = output / "chunks" / f"{chunk_id}.wav"
            subprocess.run(
                ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                 "-ss", f"{start:.6f}", "-t", f"{end - start:.6f}",
                 "-i", str(source / f"{grid.stem}.wav"), "-ac", "1",
                 "-ar", "16000", "-c:a", "pcm_s16le", "-y", str(wav)],
                check=True,
            )
            text = " ".join(item["text"] for item in items)
            acoustic = " ".join(
                spoken
                for item in items
                for token in item["text"].split()
                for spoken in NEMO_ALIGN.spoken(token, "fr")
            )
            gold[chunk_id] = {
                "source": grid.stem,
                "offset": start,
                "duration": end - start,
                "words": [
                    {"text": item["text"], "start": item["start"] - start,
                     "end": item["end"] - start, "scored": item["scored"]}
                    for item in items
                ],
            }
            line = {"audio_filepath": str(wav.resolve()), "duration": end - start}
            lines["nemo"].append({**line, "text": acoustic})
            lines["qwen"].append({**line, "text": text})
            shutil.copy(wav, output / "mfa-corpus" / wav.name)
            (output / "mfa-corpus" / f"{chunk_id}.lab").write_text(
                text + "\n", encoding="utf-8"
            )
    BENCHMARK.write_json(output / "gold.json", gold)
    for name, entries in lines.items():
        BENCHMARK.write_text(
            output / f"{name}.jsonl",
            "".join(json.dumps(e, ensure_ascii=False) + "\n" for e in entries),
        )
    words = [w for chunk in gold.values() for w in chunk["words"]]
    return {
        "chunks": len(gold),
        "minutes": sum(c["duration"] for c in gold.values()) / 60,
        "words": len(words),
        "scored_words": sum(w["scored"] for w in words),
        "longest_chunk_seconds": max(c["duration"] for c in gold.values()),
    }


def predicted_entries(run: Path, method: str, chunk_id: str) -> list | None:
    if method == "mfa":
        path = run / "mfa" / f"{chunk_id}.json"
        return BENCHMARK.mfa_entries(path, None) if path.exists() else None
    path = (
        run / "nemo" / "ctm" / "words" / f"{chunk_id}.ctm"
        if method == "nemo"
        else run / "qwen" / f"{chunk_id}.ctm"
    )
    return BENCHMARK.ctm_entries(path) if path.exists() else None


def edge_rows(
    gold: dict[str, Any], run: Path, method: str
) -> tuple[list[dict[str, Any]], list[str]]:
    rows, missing = [], []
    for chunk_id, chunk in gold.items():
        entries = predicted_entries(run, method, chunk_id)
        if entries is None:
            missing.append(chunk_id)
            continue
        raw = [{"text": t, "start": float(s), "end": float(e)} for t, s, e in entries]
        words = chunk["words"]
        pairs = dict(BENCHMARK.matched_word_indices(words, raw))
        for index, word in enumerate(words):
            if not word["scored"]:
                continue
            before = words[index - 1]["end"] if index else 0.0
            after = (
                words[index + 1]["start"]
                if index + 1 < len(words)
                else chunk["duration"]
            )
            row = {
                "chunk": chunk_id,
                "index": index,
                "style": chunk["source"][:3],
                "filler": BENCHMARK.folded_word(word["text"]) in FILLERS,
                "pause_before": word["start"] - before >= CUT_EDGE_PAUSE_SECONDS,
                "pause_after": after - word["end"] >= CUT_EDGE_PAUSE_SECONDS,
                "paired": index in pairs,
            }
            if index in pairs:
                observed = raw[pairs[index]]
                row["start_ms"] = 1000 * (observed["start"] - word["start"])
                row["end_ms"] = 1000 * (observed["end"] - word["end"])
            rows.append(row)
    return rows, missing


def summary(rows: list[dict[str, Any]]) -> dict[str, Any] | None:
    paired = [r for r in rows if r["paired"]]
    if not paired:
        return None
    starts = [r["start_ms"] for r in paired]
    ends = [r["end_ms"] for r in paired]
    edges = sorted(abs(value) for value in starts + ends)
    return {
        "scored_words": len(rows),
        "paired_words": len(paired),
        "mae_ms": statistics.fmean(edges),
        "median_ms": statistics.median(edges),
        "p95_ms": BENCHMARK.percentile(edges, 0.95),
        "start_bias_ms": statistics.fmean(starts),
        "end_bias_ms": statistics.fmean(ends),
        "edges_within_20ms_percent": 100 * sum(e <= 20 for e in edges) / len(edges),
        "edges_within_one_frame_percent": 100
        * sum(e <= TOLERANCE_MS for e in edges)
        / len(edges),
        "both_within_one_frame_percent": 100
        * sum(
            abs(r["start_ms"]) <= TOLERANCE_MS and abs(r["end_ms"]) <= TOLERANCE_MS
            for r in paired
        )
        / len(paired),
    }


def cut_edges(rows: list[dict[str, Any]]) -> dict[str, Any] | None:
    values = [abs(r["end_ms"]) for r in rows if r["paired"] and r["pause_after"]]
    values += [abs(r["start_ms"]) for r in rows if r["paired"] and r["pause_before"]]
    if not values:
        return None
    return {
        "edges": len(values),
        "mae_ms": statistics.fmean(values),
        "within_one_frame_percent": 100
        * sum(v <= TOLERANCE_MS for v in values)
        / len(values),
    }


def score(run: Path, draws: int = 2000) -> dict[str, Any]:
    gold = BENCHMARK.load_json(run / "gold.json")
    rows = {}
    result: dict[str, Any] = {"schema_version": 1, "status": "cprom_word_tier"}
    for method in METHODS:
        method_rows, missing = edge_rows(gold, run, method)
        rows[method] = method_rows
        styles = sorted({r["style"] for r in method_rows})
        result[method] = {
            "missing_chunks": missing,
            "all": summary(method_rows),
            "formal": summary([r for r in method_rows if r["style"] in FORMAL_STYLES]),
            "spontaneous": summary(
                [r for r in method_rows if r["style"] not in FORMAL_STYLES]
            ),
            "fillers": summary([r for r in method_rows if r["filler"]]),
            "cut_edges": cut_edges(method_rows),
            "by_style": {
                style: summary([r for r in method_rows if r["style"] == style])
                for style in styles
            },
        }
    # Paired bootstrap over chunks, on the words every aligner paired.
    keyed = {
        method: {(r["chunk"], r["index"]): r for r in method_rows if r["paired"]}
        for method, method_rows in rows.items()
    }
    common = set.intersection(*(set(values) for values in keyed.values()))
    chunks = sorted({key[0] for key in common})
    by_chunk = {chunk: [k for k in common if k[0] == chunk] for chunk in chunks}

    def mae(method: str, keys: list[tuple[str, int]]) -> float:
        rows_ = keyed[method]
        return statistics.fmean(
            (abs(rows_[k]["start_ms"]) + abs(rows_[k]["end_ms"])) / 2 for k in keys
        )

    generator = random.Random(20261101)
    every = [key for chunk in chunks for key in by_chunk[chunk]]
    for left, right in (("qwen", "mfa"), ("nemo", "mfa"), ("nemo", "qwen")):
        differences = []
        for _ in range(draws):
            keys = [
                key
                for chunk in generator.choices(chunks, k=len(chunks))
                for key in by_chunk[chunk]
            ]
            differences.append(mae(left, keys) - mae(right, keys))
        differences.sort()
        result[f"{left}_minus_{right}_mae_ms"] = {
            "words": len(every),
            "value": mae(left, every) - mae(right, every),
            "ci95": [
                differences[int(0.025 * draws)],
                differences[int(0.975 * draws) - 1],
            ],
        }
    return result


# Transcripts. Hesitation spellings fold to one form before comparing:
# « heu » and « euh » are the same sound, « hum » and « mh » too.
HESITATION_FORMS = {"euh": "euh", "heu": "euh", "hum": "mh", "hm": "mh",
                    "mh": "mh", "mhm": "mh"}
DISFLUENCIES = {"euh", "mh", "ben", "bah", "hein"}


def transcript_token(token: str) -> str:
    return HESITATION_FORMS.get(BENCHMARK.folded_word(token), token)


def transcript_tokens(text: str) -> list[str]:
    """Words as NeMo's normalization spells them: casefolded, digits read out,
    punctuation dropped, so a reference and a transcript compare word for
    word."""
    return [
        transcript_token(spoken)
        for token in text.split()
        for spoken in NEMO_ALIGN.spoken(token, "fr")
    ]


def reference_tokens(words: list[dict[str, Any]]) -> list[dict[str, Any]]:
    tokens = []
    for word in words:
        truncated = word["text"].endswith("-")
        for token in transcript_tokens(word["text"]):
            tokens.append({"token": token, "truncated": truncated})
    for index, token in enumerate(tokens):
        token["disfluency"] = token["token"] in DISFLUENCIES
        token["repeat"] = (
            index > 0
            and not token["disfluency"]
            and token["token"] == tokens[index - 1]["token"]
        )
    return tokens


def transcript_counts(
    reference: list[dict[str, Any]], hypothesis: list[str]
) -> dict[str, int]:
    words = [token["token"] for token in reference]
    alignment = BENCHMARK.levenshtein_alignment(words, hypothesis)
    operation = {ref: (kind, hyp) for kind, ref, hyp in alignment if ref is not None}
    counts = {key: 0 for key in (
        "reference", "substitution", "deletion", "insertion", "disfluencies",
        "disfluencies_written", "hypothesis_disfluencies",
        "hypothesis_disfluencies_matched", "euh", "euh_written", "repeats",
        "repeats_kept", "truncated", "truncated_kept")}
    counts["reference"] = len(words)
    for kind, _, _ in alignment:
        if kind != "correct":
            counts[kind] += 1
    matched_hypothesis = {hyp for kind, _, hyp in alignment if kind == "correct"}
    for index, token in enumerate(reference):
        kind, hyp = operation[index]
        correct = kind == "correct"
        if token["disfluency"]:
            counts["disfluencies"] += 1
            counts["disfluencies_written"] += correct
            if token["token"] == "euh":
                counts["euh"] += 1
                counts["euh_written"] += correct
        if token["repeat"]:
            counts["repeats"] += 1
            counts["repeats_kept"] += correct and operation[index - 1][0] == "correct"
        if token["truncated"]:
            counts["truncated"] += 1
            counts["truncated_kept"] += correct or (
                kind == "substitution"
                and hypothesis[hyp].startswith(token["token"])
            )
    for index, token in enumerate(hypothesis):
        if token in DISFLUENCIES:
            counts["hypothesis_disfluencies"] += 1
            counts["hypothesis_disfluencies_matched"] += index in matched_hypothesis
    content = [token for token in reference if not token["disfluency"]]
    content_alignment = BENCHMARK.levenshtein_alignment(
        [token["token"] for token in content],
        [token for token in hypothesis if token not in DISFLUENCIES],
    )
    counts["content_reference"] = len(content)
    counts["content_errors"] = sum(
        1 for kind, _, _ in content_alignment if kind != "correct"
    )
    return counts


def transcript_summary(counts: list[dict[str, int]]) -> dict[str, Any] | None:
    if not counts:
        return None
    total = {key: sum(c[key] for c in counts) for key in counts[0]}

    def percent(part: str, whole: str) -> float | None:
        return 100 * total[part] / total[whole] if total[whole] else None

    errors = total["substitution"] + total["deletion"] + total["insertion"]
    return {
        **total,
        "wer_percent": 100 * errors / total["reference"],
        "content_wer_percent": percent("content_errors", "content_reference"),
        "disfluencies_written_percent": percent(
            "disfluencies_written", "disfluencies"
        ),
        "euh_written_percent": percent("euh_written", "euh"),
        "repeats_kept_percent": percent("repeats_kept", "repeats"),
        "truncated_kept_percent": percent("truncated_kept", "truncated"),
        "hypothesis_disfluencies_matched_percent": percent(
            "hypothesis_disfluencies_matched", "hypothesis_disfluencies"
        ),
    }


def score_transcripts(run: Path, hypotheses: dict[str, Path]) -> dict[str, Any]:
    gold = BENCHMARK.load_json(run / "gold.json")
    references = {
        chunk_id: reference_tokens(chunk["words"]) for chunk_id, chunk in gold.items()
    }
    result: dict[str, Any] = {"schema_version": 1, "status": "cprom_transcripts"}
    for label, path in hypotheses.items():
        texts = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                entry = json.loads(line)
                texts[entry["stem"]] = entry["text"]
        missing = sorted(set(gold) - set(texts))
        per_chunk = {
            chunk_id: transcript_counts(
                references[chunk_id], transcript_tokens(texts[chunk_id])
            )
            for chunk_id in gold
            if chunk_id in texts
        }
        style = {chunk_id: gold[chunk_id]["source"][:3] for chunk_id in per_chunk}
        result[label] = {
            "missing_chunks": missing,
            "all": transcript_summary(list(per_chunk.values())),
            "formal": transcript_summary(
                [c for k, c in per_chunk.items() if style[k] in FORMAL_STYLES]
            ),
            "spontaneous": transcript_summary(
                [c for k, c in per_chunk.items() if style[k] not in FORMAL_STYLES]
            ),
        }
    return result


# Phones. Does a phone recognizer hear the hand-placed « euh »? A phone of
# the « euh » class inside one counts; the same class emitted outside every
# gold word is what a detector reading pauses would raise in vain.
HESITATION_PHONES = {"ø", "œ", "ə"}
PHONE_MARGIN_SECONDS = 0.02


def phone_counts(
    words: list[dict[str, Any]], phones: list[list[Any]]
) -> dict[str, int]:
    counts = {"euh": 0, "euh_any_phone": 0, "euh_hesitation_phone": 0,
              "phones_outside_words": 0, "hesitation_phones_outside_words": 0}

    def base(symbol: str) -> str:
        return symbol.replace("ː", "").replace("ˑ", "")

    for word in words:
        if word["text"] != "euh":
            continue
        inside = [
            phone for phone in phones
            if phone[2] > word["start"] - PHONE_MARGIN_SECONDS
            and phone[1] < word["end"] + PHONE_MARGIN_SECONDS
        ]
        counts["euh"] += 1
        counts["euh_any_phone"] += bool(inside)
        counts["euh_hesitation_phone"] += any(
            base(phone[0]) in HESITATION_PHONES for phone in inside
        )
    for symbol, start, end in phones:
        middle = (start + end) / 2
        if not any(w["start"] <= middle < w["end"] for w in words):
            counts["phones_outside_words"] += 1
            counts["hesitation_phones_outside_words"] += (
                base(symbol) in HESITATION_PHONES
            )
    return counts


def score_phones(run: Path, recognizers: dict[str, Path]) -> dict[str, Any]:
    gold = BENCHMARK.load_json(run / "gold.json")
    result: dict[str, Any] = {"schema_version": 1, "status": "cprom_phones"}
    for label, path in recognizers.items():
        phones = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                entry = json.loads(line)
                phones[entry["stem"]] = entry["phones"]
        per_chunk = [
            phone_counts(chunk["words"], phones[chunk_id])
            for chunk_id, chunk in gold.items()
            if chunk_id in phones
        ]
        total = {key: sum(c[key] for c in per_chunk) for key in per_chunk[0]}
        total["missing_chunks"] = sorted(set(gold) - set(phones))
        for key in ("euh_any_phone", "euh_hesitation_phone"):
            total[f"{key}_percent"] = (
                100 * total[key] / total["euh"] if total["euh"] else None
            )
        result[label] = total
    return result


# Filled pauses. C-PROM marks every syllable affected by a hesitation (« z »
# in its delivery tier): a « euh », or a vowel held while the speaker looks
# for the next word (« leeeee »). Both are what an editor cuts; a detector is
# scored on both, split by whether the syllable lies on a written « euh ».
HESITATION_MARK = "z"
DETECTION_OVERLAP_SECONDS = 0.04


def is_tuning_recording(source: str) -> bool:
    """Speaker-disjoint split: Belgian and Swiss speakers and the first
    three itineraries tune a detector, French speakers and the other
    itineraries test it."""
    return source.endswith(("-be", "-ch")) or source in {
        "iti-01", "iti-02", "iti-03"}


def hesitation_targets(source: Path, gold: dict[str, Any]) -> dict[str, Any]:
    targets: dict[str, Any] = {
        chunk_id: {"euh": [], "lengthening": []} for chunk_id in gold
    }
    chunks_by_source: dict[str, list[str]] = {}
    for chunk_id, chunk in gold.items():
        chunks_by_source.setdefault(chunk["source"], []).append(chunk_id)
    for name, chunk_ids in chunks_by_source.items():
        tiers = BENCHMARK.praat_binary_tiers(source / f"{name}.TextGrid")
        euh_words = [(s, e) for s, e, label in tiers["words"]
                     if label.strip() == "euh"]
        for (start, end, _), (_, _, mark) in zip(tiers["syll"], tiers["delivery"]):
            if HESITATION_MARK not in mark:
                continue
            kind = (
                "euh"
                if any(s < end and start < e for s, e in euh_words)
                else "lengthening"
            )
            for chunk_id in chunk_ids:
                offset = gold[chunk_id]["offset"]
                if offset <= start < offset + gold[chunk_id]["duration"]:
                    targets[chunk_id][kind].append([start - offset, end - offset])
                    break
    return targets


def overlap(first: list[float], second: list[float]) -> float:
    return max(0.0, min(first[1], second[1]) - max(first[0], second[0]))


def filled_pause_counts(
    targets: dict[str, list[list[float]]], detections: list[list[float]]
) -> dict[str, int]:
    """A target is found when a detection covers half of it or 100 ms; a
    detection that meets no target by 40 ms is a false alarm."""
    counts = {"euh": 0, "euh_found": 0, "lengthening": 0,
              "lengthening_found": 0, "detections": 0, "false_alarms": 0}
    for kind in ("euh", "lengthening"):
        for target in targets[kind]:
            counts[kind] += 1
            needed = min(0.1, 0.5 * (target[1] - target[0]))
            counts[f"{kind}_found"] += any(
                overlap(target, d) >= needed for d in detections
            )
    every = targets["euh"] + targets["lengthening"]
    for detection in detections:
        counts["detections"] += 1
        counts["false_alarms"] += not any(
            overlap(detection, t) >= DETECTION_OVERLAP_SECONDS for t in every
        )
    return counts


def filled_pause_summary(
    counts: list[dict[str, int]], minutes: float
) -> dict[str, Any]:
    total = {key: sum(c[key] for c in counts) for key in counts[0]}
    found = total["euh_found"] + total["lengthening_found"]
    targets = total["euh"] + total["lengthening"]
    recall = found / targets if targets else 0.0
    precision = (
        1 - total["false_alarms"] / total["detections"]
        if total["detections"]
        else 0.0
    )
    return {
        **total,
        "minutes": minutes,
        "euh_found_percent": 100 * total["euh_found"] / total["euh"]
        if total["euh"] else None,
        "lengthening_found_percent": 100 * total["lengthening_found"]
        / total["lengthening"] if total["lengthening"] else None,
        "recall_percent": 100 * recall,
        "precision_percent": 100 * precision,
        "f1": 2 * recall * precision / (recall + precision)
        if recall + precision else 0.0,
        "false_alarms_per_minute": total["false_alarms"] / minutes
        if minutes else None,
    }


def score_filled_pauses(
    run: Path, source: Path, detectors: dict[str, Path]
) -> dict[str, Any]:
    gold = BENCHMARK.load_json(run / "gold.json")
    targets = hesitation_targets(source, gold)
    result: dict[str, Any] = {"schema_version": 1, "status": "cprom_filled_pauses"}
    for label, path in detectors.items():
        detections = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                entry = json.loads(line)
                detections[entry["stem"]] = entry["segments"]
        per_split: dict[str, Any] = {}
        for split, keep in (
            ("tuning", lambda c: is_tuning_recording(gold[c]["source"])),
            ("test", lambda c: not is_tuning_recording(gold[c]["source"])),
            ("all", lambda c: True),
        ):
            chunk_ids = [c for c in gold if keep(c) and c in detections]
            per_split[split] = filled_pause_summary(
                [filled_pause_counts(targets[c], detections[c]) for c in chunk_ids],
                sum(gold[c]["duration"] for c in chunk_ids) / 60,
            )
        per_split["missing_chunks"] = sorted(set(gold) - set(detections))
        result[label] = per_split
    return result


# Dropped hesitations. The transcript a model wrote, aligned by MFA, misses
# most « euh ». Two finders are compared on those: the engine's
# voice-outside-words check (A18) on the words as the engine widens them,
# and the filled-pause detector, kept where no aligned phone of ordinary
# length explains it (in a gap, or over a phone MFA stretched to absorb the
# « euh » into a neighbour).
LONG_PHONE_SECONDS = 0.30
WRITTEN_HESITATIONS = {"euh", "heu"}
NOT_PHONES = {"", "sil", "spn"}


def dropped_hesitation_counts(
    chunk: dict[str, Any],
    targets: dict[str, list[list[float]]],
    tiers: dict[str, Any],
    detections: list[list[float]],
    voiced: list[bool],
    long_phone: float = LONG_PHONE_SECONDS,
) -> dict[str, int]:
    words = [[s, e, t] for s, e, t in tiers["words"]["entries"]]
    phones = [
        [s, e] for s, e, t in tiers["phones"]["entries"] if t not in NOT_PHONES
    ]
    written = [w for w in words if BENCHMARK.folded_word(w[2]) in WRITTEN_HESITATIONS]
    euhs = [[w["start"], w["end"]] for w in chunk["words"] if w["text"] == "euh"]
    dropped = [e for e in euhs if not any(overlap(e, w) > 0 for w in written)]
    every = targets["euh"] + targets["lengthening"]

    def found(target: list[float], spans: list[list[float]]) -> bool:
        needed = min(0.1, 0.5 * (target[1] - target[0]))
        return any(overlap(target, span) >= needed for span in spans)

    def unexplained(spans: list[list[float]]) -> int:
        return sum(
            1 for span in spans
            if not any(overlap(span, t) >= DETECTION_OVERLAP_SECONDS for t in every)
        )

    rate = (25, 1)
    quantized = BENCHMARK.quantize_words(
        [(t, Decimal(str(s)), Decimal(str(e))) for s, e, t in words], rate
    )
    holes = [
        [start / 25, end / 25]
        for start, end in BENCHMARK.voice_outside_words(
            quantized, voiced, BENCHMARK.hole_minimum_frames(rate), True
        )
    ]
    ordinary = [p for p in phones if p[1] - p[0] < long_phone]
    candidates = [
        d for d in detections
        if sum(overlap(d, p) for p in ordinary) < 0.5 * (d[1] - d[0])
    ]
    return {
        "euh": len(euhs),
        "dropped": len(dropped),
        "engine_check_found": sum(found(e, holes) for e in dropped),
        "engine_check_alerts": len(holes),
        "engine_check_unexplained": unexplained(holes),
        "detector_found": sum(found(e, candidates) for e in dropped),
        "detector_alerts": len(candidates),
        "detector_unexplained": unexplained(candidates),
    }


def score_dropped_hesitations(
    run: Path, source: Path, aligned: Path, detections_path: Path,
    long_phone: float = LONG_PHONE_SECONDS,
) -> dict[str, Any]:
    gold = BENCHMARK.load_json(run / "gold.json")
    targets = hesitation_targets(source, gold)
    detections = {}
    for line in detections_path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            entry = json.loads(line)
            detections[entry["stem"]] = entry["segments"]
    per_split: dict[str, list[dict[str, int]]] = {"tuning": [], "test": []}
    minutes = {"tuning": 0.0, "test": 0.0}
    for chunk_id, chunk in gold.items():
        split = "tuning" if is_tuning_recording(chunk["source"]) else "test"
        rate = (25, 1)
        frames = BENCHMARK.frame_ceil(Decimal(str(chunk["duration"])), rate)
        _, powers = BENCHMARK.envelope_window_powers(
            run / "chunks" / f"{chunk_id}.wav"
        )
        per_split[split].append(dropped_hesitation_counts(
            chunk, targets[chunk_id],
            BENCHMARK.load_json(aligned / f"{chunk_id}.json")["tiers"],
            detections.get(chunk_id, []),
            BENCHMARK.voiced_frames(powers, rate, frames), long_phone,
        ))
        minutes[split] += chunk["duration"] / 60
    result: dict[str, Any] = {
        "schema_version": 1, "status": "cprom_dropped_hesitations",
        "long_phone_seconds": long_phone,
    }
    for split, counts in per_split.items():
        total = {key: sum(c[key] for c in counts) for key in counts[0]}
        total["minutes"] = minutes[split]
        for finder in ("engine_check", "detector"):
            total[f"{finder}_found_percent"] = (
                100 * total[f"{finder}_found"] / total["dropped"]
                if total["dropped"] else None
            )
            total[f"{finder}_unexplained_per_minute"] = (
                total[f"{finder}_unexplained"] / minutes[split]
            )
        result[split] = total
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_command = commands.add_parser("prepare")
    prepare_command.add_argument("--source", type=Path, required=True)
    prepare_command.add_argument("--output-dir", type=Path, required=True)
    score_command = commands.add_parser("score")
    score_command.add_argument("--run-dir", type=Path, required=True)
    score_command.add_argument("--output", type=Path)
    transcripts_command = commands.add_parser("score-transcripts")
    transcripts_command.add_argument("--run-dir", type=Path, required=True)
    transcripts_command.add_argument(
        "--hypotheses", nargs="+", required=True, metavar="LABEL=JSONL"
    )
    transcripts_command.add_argument("--output", type=Path)
    phones_command = commands.add_parser("score-phones")
    phones_command.add_argument("--run-dir", type=Path, required=True)
    phones_command.add_argument(
        "--phones", nargs="+", required=True, metavar="LABEL=JSONL"
    )
    phones_command.add_argument("--output", type=Path)
    pauses_command = commands.add_parser("score-filled-pauses")
    pauses_command.add_argument("--run-dir", type=Path, required=True)
    pauses_command.add_argument("--source", type=Path, required=True)
    pauses_command.add_argument(
        "--detections", nargs="+", required=True, metavar="LABEL=JSONL"
    )
    pauses_command.add_argument("--output", type=Path)
    dropped_command = commands.add_parser("score-dropped-hesitations")
    dropped_command.add_argument("--run-dir", type=Path, required=True)
    dropped_command.add_argument("--source", type=Path, required=True)
    dropped_command.add_argument("--aligned-dir", type=Path, required=True)
    dropped_command.add_argument("--detections", type=Path, required=True)
    dropped_command.add_argument(
        "--long-phone-seconds", type=float, default=LONG_PHONE_SECONDS
    )
    dropped_command.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            result = prepare(args.source, args.output_dir)
        elif args.command == "score":
            result = score(args.run_dir)
        elif args.command == "score-dropped-hesitations":
            result = score_dropped_hesitations(
                args.run_dir, args.source, args.aligned_dir, args.detections,
                args.long_phone_seconds,
            )
        else:
            items = {
                "score-transcripts": lambda: args.hypotheses,
                "score-phones": lambda: args.phones,
                "score-filled-pauses": lambda: args.detections,
            }[args.command]()
            pairs = [item.split("=", 1) for item in items]
            if any(len(pair) != 2 for pair in pairs):
                raise BenchmarkError("expected LABEL=JSONL")
            inputs = {label: Path(path) for label, path in pairs}
            if args.command == "score-transcripts":
                result = score_transcripts(args.run_dir, inputs)
            elif args.command == "score-phones":
                result = score_phones(args.run_dir, inputs)
            else:
                result = score_filled_pauses(args.run_dir, args.source, inputs)
        if args.command != "prepare" and args.output:
            BENCHMARK.write_json(args.output, result)
    except BenchmarkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
