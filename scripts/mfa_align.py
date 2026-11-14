#!/usr/bin/env python3
"""Word boundaries for interview rushes: Whisper text, MFA timing.

CUTMACHINE's verbatim Whisper cache supplies the text, Montreal Forced Aligner
the boundaries: it aligns the text it is given and never re-transcribes.

ALIGNEMENT-2026-11 -- docs/roadmap/ALIGNEMENT-2026-11.md. MFA replaced NeMo
Forced Aligner, which the bench had chosen on a 29 s pilot corrected from its
own output. Against C-PROM, 70 minutes of French aligned by EasyAlign and
checked by hand (docs/WORD_ALIGNMENT_BENCH_QWEN.md), NeMo's boundaries were
73 ms from the reference on average, MFA's 31 ms; at the edges next to a
pause, where an edit cuts, 97 ms against 42.

AX2 -- docs/roadmap/AX-2026-09.md. The engine runs this script itself:
`align_words` over MCP (a background task), `--align-words` on the command
line. Running it by hand still works, but writes the package's caches outside
the session lock the engine holds.

The result is written as a parallel cache next to the Whisper one, in the
bench's manifest format (schema_version 1, half-open [frame_start, frame_end)
on the source frame grid). The engine reads it (ITW3, WordAlignment.h): every
word-level tool, get_timeline_transcript and the SRT export use its boundaries
while its text still matches the Whisper cache, which is left untouched.

    python3 mfa_align.py PROJECT.json MEDIA [MEDIA ...] --work DIR \
        [--report REPORT.json] [--mfa PATH] [--models DIR]

MEDIA is a media ULID or a filename stem (C0042). The work directory holds the
16 kHz WAVs, MFA's corpus, output and working files (MFA_ROOT_DIR): keep it
off exFAT, and off the system disk when it is short of space.

ITW9 -- docs/roadmap/ITW-2026-09.md. A rush is aligned in the language its
text was transcribed in, with that language's MFA models (acoustic model,
pronunciation dictionary, and the G2P model that spells the words the
dictionary lacks). Handed English or Spanish, a French model was given
"quatre-vingt-quatre" where the speaker said "eighty-four".
"""

from __future__ import annotations

import argparse
import csv
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import unicodedata
import wave
from array import array
from decimal import ROUND_CEILING, ROUND_FLOOR, Decimal
from fractions import Fraction
from pathlib import Path

# Per language: MFA's acoustic model, its pronunciation dictionary and the
# G2P model for the words the dictionary lacks, as files of --models.
MODELS = {
    "fr": {"name": "french_mfa",
           "acoustic": "acoustic-french_mfa-v3.0.0.zip",
           "dictionary": "dictionary-french_mfa-v3.0.0.dict",
           "g2p": "g2p-french_mfa-v3.0.0.zip"},
    "en": {"name": "english_mfa",
           "acoustic": "acoustic-english_mfa-v3.1.0.zip",
           "dictionary": "dictionary-english_mfa-v3.1.0.dict",
           "g2p": "g2p-english_us_mfa-v3.0.0.zip"},
    "es": {"name": "spanish_mfa",
           "acoustic": "acoustic-spanish_mfa-v3.3.0.zip",
           "dictionary": "dictionary-spanish_spain_mfa-v3.3.0.dict",
           "g2p": "g2p-spanish_spain_mfa-v3.3.0.zip"},
}
# A whole rush is one utterance for MFA: its default beam leaves a two-minute
# take unaligned without an error (docs/WORD_ALIGNMENT_BENCH_QWEN.md).
MFA_BEAM = "100"
MFA_RETRY_BEAM = "400"
# Every rush is first aligned whole: MFA aligned a twelve-minute file of
# C-PROM whole (34.8 ms from the reference), and Whisper's own timestamps
# drift on a long rush, so cutting it by them hands MFA text that belongs
# elsewhere -- on that file transcribed by the engine, two utterances in
# nineteen got the words of the minute before and were left unaligned, where
# the whole rush aligned. A rush longer than this that MFA leaves unaligned
# whole is tried again as utterances cut in the middle of its silences, each
# at least SEGMENT_SECONDS long.
WHOLE_RUSH_SECONDS = 90.0
SEGMENT_SECONDS = 30.0
CUT_SILENCE_SECONDS = 0.3
# The engine's voice line (SpeechOnsetThresholds): 25 % of the 90th
# percentile amplitude, on 20 ms windows.
VOICE_WINDOW_SAMPLES = 320
VOICE_LINE_RATIO = 0.25
VOICE_PERCENTILE = 90
FILLER_WORDS = {
    "fr": {"euh", "heu", "hum", "hm", "mh", "ben", "bah", "beh", "hein"},
    "en": {"uh", "um", "er", "erm", "hm", "mh", "mhm", "ah"},
    "es": {"eh", "em", "ehm", "hm", "mh", "ah"},
}
# The letters each language's text is spelt with before it reaches the
# aligner: lowercase Latin plus French diacritics, ' and - for French; '
# alone for English; the Spanish accents, ñ and ü, with neither ' nor -, for
# Spanish. Chosen for NeMo's vocabularies (ITW9) and kept: the cache file is
# named after this text's hash.
VOCABS = {
    "fr": set("abcdefghijklmnopqrstuvwxyzàâæçèéêëìîïôùûüÿœ'-"),
    "en": set("abcdefghijklmnopqrstuvwxyz'"),
    "es": set("abcdefghijklmnopqrstuvwxyzáéíñóúü"),
}
# What a transcript made with language "auto" and no recorded detection is
# guessed to be, from words only one of the three languages uses; French
# when they do not decide, as before ITW9.
DISTINCT_WORDS = {
    "fr": {"le", "les", "des", "du", "et", "est", "je", "vous", "nous", "qui",
           "pas", "dans", "pour", "sur", "avec", "ce", "il", "elle", "on",
           "au", "aux", "ne", "mais", "très", "aussi", "sont", "c'est",
           "j'ai", "être", "avoir", "fait", "notre", "votre"},
    "en": {"the", "and", "is", "are", "of", "to", "you", "we", "in", "that",
           "it", "for", "with", "this", "on", "be", "will", "was", "have",
           "not", "at", "our", "your", "from", "can", "by", "an", "it's",
           "i'm", "my", "they"},
    "es": {"el", "los", "las", "del", "y", "es", "una", "para", "con", "por",
           "no", "su", "tu", "lo", "al", "como", "más", "pero", "muy",
           "también", "nuestro", "nuestra", "son", "está", "soy", "me",
           "puedes", "tendrás", "vas"},
}
TOKEN = re.compile(r"[^\W_]+(?:['’\-][^\W_]+)*")

UNITS = ["zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept",
         "huit", "neuf", "dix", "onze", "douze", "treize", "quatorze",
         "quinze", "seize"]
TENS = {20: "vingt", 30: "trente", 40: "quarante", 50: "cinquante",
        60: "soixante"}


def below_hundred(n: int) -> str:
    if n <= 16:
        return UNITS[n]
    if n < 20:
        return "dix-" + UNITS[n - 10]
    if n < 70:
        tens, unit = divmod(n, 10)
        word = TENS[tens * 10]
        if unit == 0:
            return word
        return word + ("-et-un" if unit == 1 else "-" + UNITS[unit])
    if n < 80:
        return "soixante" + ("-et-" if n == 71 else "-") + below_hundred(n - 60)
    if n == 80:
        return "quatre-vingts"
    return "quatre-vingt-" + below_hundred(n - 80)


def cardinal(n: int) -> str:
    if n < 100:
        return below_hundred(n)
    if n < 1000:
        hundreds, rest = divmod(n, 100)
        head = "cent" if hundreds == 1 else UNITS[hundreds] + " cent"
        if rest == 0:
            return head + ("s" if hundreds > 1 else "")
        return head + " " + below_hundred(rest)
    if n < 1_000_000:
        thousands, rest = divmod(n, 1000)
        head = "mille" if thousands == 1 else cardinal(thousands) + " mille"
        return head if rest == 0 else head + " " + cardinal(rest)
    return " ".join(UNITS[int(d)] for d in str(n))


def ordinal(n: int) -> str:
    if n == 1:
        return "premier"
    word = cardinal(n)
    if word.endswith("cinq"):
        return word + "uième"
    if word.endswith("neuf"):
        return word[:-1] + "vième"
    if word.endswith("e"):
        word = word[:-1]
    return word + "ième"


EN_UNITS = ["zero", "one", "two", "three", "four", "five", "six", "seven",
            "eight", "nine", "ten", "eleven", "twelve", "thirteen",
            "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
            "nineteen"]
EN_TENS = {2: "twenty", 3: "thirty", 4: "forty", 5: "fifty", 6: "sixty",
           7: "seventy", 8: "eighty", 9: "ninety"}
EN_ORDINALS = {"one": "first", "two": "second", "three": "third",
               "five": "fifth", "eight": "eighth", "nine": "ninth",
               "twelve": "twelfth"}


def english_below_hundred(n: int) -> str:
    if n < 20:
        return EN_UNITS[n]
    tens, unit = divmod(n, 10)
    return EN_TENS[tens] + ("" if unit == 0 else " " + EN_UNITS[unit])


def english_cardinal(n: int) -> str:
    if n < 100:
        return english_below_hundred(n)
    if n < 1000:
        hundreds, rest = divmod(n, 100)
        head = EN_UNITS[hundreds] + " hundred"
        return head if rest == 0 else head + " " + english_below_hundred(rest)
    if n < 1_000_000:
        thousands, rest = divmod(n, 1000)
        head = english_cardinal(thousands) + " thousand"
        return head if rest == 0 else head + " " + english_cardinal(rest)
    return " ".join(EN_UNITS[int(d)] for d in str(n))


def english_number(digits: str) -> str:
    """A number as an English speaker reads it: four digits from 1100 to
    2099 are a year, said in pairs ("twenty twenty-seven"), except the
    "two thousand five" of the first decade."""
    n = int(digits)
    if len(digits) == 4 and 1100 <= n <= 2099 and not 2000 <= n <= 2009:
        century, rest = divmod(n, 100)
        if rest == 0:
            return english_below_hundred(century) + " hundred"
        return (english_below_hundred(century) + " " +
                ("oh " + EN_UNITS[rest] if rest < 10
                 else english_below_hundred(rest)))
    return english_cardinal(n)


def english_ordinal(n: int) -> str:
    words = english_cardinal(n).split()
    last = words[-1]
    if last in EN_ORDINALS:
        last = EN_ORDINALS[last]
    elif last.endswith("y"):
        last = last[:-1] + "ieth"
    else:
        last += "th"
    return " ".join(words[:-1] + [last])


ES_UNITS = ["cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete",
            "ocho", "nueve", "diez", "once", "doce", "trece", "catorce",
            "quince", "dieciséis", "diecisiete", "dieciocho", "diecinueve",
            "veinte", "veintiuno", "veintidós", "veintitrés", "veinticuatro",
            "veinticinco", "veintiséis", "veintisiete", "veintiocho",
            "veintinueve"]
ES_TENS = {3: "treinta", 4: "cuarenta", 5: "cincuenta", 6: "sesenta",
           7: "setenta", 8: "ochenta", 9: "noventa"}
ES_HUNDREDS = {1: "ciento", 2: "doscientos", 3: "trescientos",
               4: "cuatrocientos", 5: "quinientos", 6: "seiscientos",
               7: "setecientos", 8: "ochocientos", 9: "novecientos"}
ES_ORDINALS = ["", "primero", "segundo", "tercero", "cuarto", "quinto",
               "sexto", "séptimo", "octavo", "noveno", "décimo"]


def spanish_below_hundred(n: int) -> str:
    if n < 30:
        return ES_UNITS[n]
    tens, unit = divmod(n, 10)
    return ES_TENS[tens] + ("" if unit == 0 else " y " + ES_UNITS[unit])


def spanish_cardinal(n: int) -> str:
    if n < 100:
        return spanish_below_hundred(n)
    if n == 100:
        return "cien"
    if n < 1000:
        hundreds, rest = divmod(n, 100)
        head = ES_HUNDREDS[hundreds]
        return head if rest == 0 else head + " " + spanish_below_hundred(rest)
    if n < 1_000_000:
        thousands, rest = divmod(n, 1000)
        head = "mil" if thousands == 1 else spanish_cardinal(thousands) + " mil"
        return head if rest == 0 else head + " " + spanish_cardinal(rest)
    return " ".join(ES_UNITS[int(d)] for d in str(n))


def spanish_ordinal(n: int, suffix: str) -> str:
    """1.º, 1.ª, 1.er: the first ten have their own word, in the gender of
    the suffix; past ten a Spanish speaker says the cardinal."""
    if not 1 <= n <= 10:
        return spanish_cardinal(n)
    word = ES_ORDINALS[n]
    if suffix == "er" and n in (1, 3):
        return word[:-1]
    if suffix == "a":
        return word[:-1] + "a"
    return word


def spoken_number(digits: str, suffix: str | None, language: str) -> str:
    n = int(digits)
    if language == "en":
        return english_ordinal(n) if suffix else english_number(digits)
    if language == "es":
        return spanish_ordinal(n, suffix) if suffix else spanish_cardinal(n)
    return ordinal(n) if suffix else cardinal(n)


# The ordinal suffixes each language writes after digits. NFKC, applied to
# the text first, has already turned º and ª into o and a.
ORDINAL_SUFFIXES = {
    "fr": r"(er|re|e|è|ème|eme|ième|nd|nde)",
    "en": r"(st|nd|rd|th)",
    "es": r"(o|a|er)",
}


def spoken(token: str, language: str = "fr") -> list[str]:
    """Acoustic words the aligner is given for one lexical token; may be
    empty."""
    vocab = VOCABS[language]
    token = token.casefold().replace("’", "'")
    match = re.fullmatch(r"(\d+)" + ORDINAL_SUFFIXES[language] + "?", token)
    if match:
        return spoken_number(match.group(1), match.group(2), language).split()
    if any(c.isdigit() for c in token):
        token = re.sub(r"\d+", lambda m: " " + spoken_number(
            m.group(), None, language) + " ", token)
    letters = []
    for character in token:
        if character in vocab or character.isspace():
            letters.append(character)
            continue
        # A model without - in its vocabulary hears two words in
        # "eighty-four" or "well-known", not one.
        if character == "-":
            letters.append(" ")
            continue
        base = unicodedata.normalize("NFKD", character)[0]
        if base in vocab:
            letters.append(base)
    text = "".join(letters)
    # Bench normalization: collapse runs of three or more identical letters.
    text = re.sub(r"(.)\1{2,}", r"\1", text)
    return [w.strip("'-") for w in text.split() if w.strip("'-")]


def frame_floor(seconds: Decimal, rate: tuple[int, int]) -> int:
    value = seconds * Decimal(rate[0]) / Decimal(rate[1])
    return int(value.to_integral_value(rounding=ROUND_FLOOR))


def frame_ceil(seconds: Decimal, rate: tuple[int, int]) -> int:
    value = seconds * Decimal(rate[0]) / Decimal(rate[1])
    return int(value.to_integral_value(rounding=ROUND_CEILING))


def rounded_ms(frames: int, rate: tuple[int, int]) -> int:
    value = Fraction(frames * 1000 * rate[1], rate[0])
    return (2 * value.numerator + value.denominator) // (2 * value.denominator)


def quantize(entries, rate, duration_frames, language="fr"):
    """The bench's policy: start floored, end ceiled, no overlap, and a word
    left without a frame of its own is merged into the next one."""
    words, pending = [], []
    for text, start_s, end_s in entries:
        start = frame_floor(start_s, rate)
        end = min(frame_ceil(end_s, rate), duration_frames)
        if words:
            start = max(start, words[-1]["frame_end"])
        if end <= start:
            pending.append(text)
            continue
        if pending:
            text = " ".join(pending + [text])
            pending.clear()
        words.append({"text": text, "frame_start": start, "frame_end": end,
                      "confidence": None, "speaker_id": None})
    if pending and words:
        words[-1]["text"] += " " + " ".join(pending)
    for index, word in enumerate(words):
        folded = "".join(c for c in unicodedata.normalize(
            "NFKD", word["text"].casefold()) if c.isalpha())
        word["filler_word"] = (re.sub(r"(.)\1+", r"\1", folded)
                              in FILLER_WORDS[language])
        following = (words[index + 1]["frame_start"]
                     if index + 1 < len(words) else word["frame_end"])
        silence = max(0, following - word["frame_end"])
        word["silence_after_frames"] = silence
        word["silence_after_ms"] = rounded_ms(silence, rate)
    return words


def uncovered_speech(cache: Path, media: dict, items: list[dict],
                     rate: tuple[int, int]) -> list[tuple[int, int]]:
    """Speech groups (B1 envelope) that no Whisper word covers, in source
    frames, when at least one second long. The aligner aligns only the text
    it is given, so each of these drags the neighbouring words into it."""
    onset = cache / "speechonset" / f"{media['id']}.json"
    if not onset.exists():
        return []
    envelope = json.loads(onset.read_text(encoding="utf-8"))
    per_frame = Fraction(envelope["windows_per_second"]) * rate[1] / rate[0]
    covered = []
    for item in items:
        if not any(c.isalpha() for c in item["text"]) or "start" not in item:
            continue
        start = Fraction(item["start"]["value"] * rate[0],
                         item["start"]["rate"] * rate[1])
        end = Fraction(item["end"]["value"] * rate[0],
                       item["end"]["rate"] * rate[1])
        covered.append((start - 5, end + 5))
    holes = []
    for group in envelope["groups"]:
        if group["level_dbfs"] < -30:
            continue
        begin = int(Fraction(group["start"]["value"]) / per_frame)
        finish = int(Fraction(group["end"]["value"]) / per_frame)
        run = None
        for frame in range(begin, finish + 1):
            inside = frame < finish and any(a <= frame < b for a, b in covered)
            if not inside and frame < finish:
                run = frame if run is None else run
            elif run is not None:
                if frame - run >= rate[0] // rate[1]:
                    holes.append((run, frame))
                run = None
    return holes


def guess_language(text: str) -> str:
    """The language of a text among the three aligned, from the words that
    belong to one of them only; French unless one clearly leads."""
    shared = set()
    seen = set()
    for words in DISTINCT_WORDS.values():
        shared |= seen & words
        seen |= words
    scores = dict.fromkeys(DISTINCT_WORDS, 0)
    for match in TOKEN.finditer(text.casefold().replace("’", "'")):
        for language, words in DISTINCT_WORDS.items():
            if match.group() in words and match.group() not in shared:
                scores[language] += 1
    ranked = sorted(scores.items(), key=lambda item: item[1], reverse=True)
    best, runner_up = ranked[0], ranked[1]
    if best[1] >= 2 and best[1] >= 2 * runner_up[1]:
        return best[0]
    return "fr"


def alignment_language(transcript: dict) -> tuple[str, str]:
    """The language a rush is aligned in, and where it was read: the one
    Whisper was told to transcribe in, else the one it detected (recorded
    since ITW9), else the one its words point to. May name a language no
    model is listed for."""
    requested = transcript.get("language") or "auto"
    if requested != "auto":
        return requested, "transcript"
    detected = transcript.get("detected_language")
    if detected:
        return detected, "detected"
    text = " ".join(item.get("text", "") for item in transcript["words"])
    return guess_language(text), "text"


def load_library(project: Path) -> list[dict]:
    root = json.loads(project.read_text(encoding="utf-8"))
    # S7 -- SAVING_ROADMAP.md: version 3 stores the shared library once.
    if root.get("project_version") == 3:
        return root["library"]
    snapshot = json.loads(root["timeline_snapshots"][0])
    return snapshot["library"]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("project", type=Path)
    parser.add_argument("media", nargs="+")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--text", type=Path, help=(
        "JSON {media: texte} replacing the Whisper text of a rush whose cache "
        "leaves measured speech without words. MFA aligns the text it is "
        "given: a missing passage drags the neighbouring words into it."))
    parser.add_argument("--report", type=Path, help=(
        "JSON summary for the engine: the rushes aligned (words, cache "
        "file), the ones skipped and why, and the speech no word covers."))
    repo = Path(__file__).resolve().parents[1]
    parser.add_argument("--mfa", default=os.environ.get(
        "CUTMACHINE_MFA_ALIGNER", str(repo / "build/mfa/env/bin/mfa")))
    parser.add_argument("--models", type=Path, help=(
        "the directory of MFA's model files (MODELS); by default models/ "
        "beside the environment that holds --mfa"))
    parser.add_argument("--jobs", type=int,
                        default=max(1, min(4, os.cpu_count() or 1)))
    args = parser.parse_args(argv)
    models = (args.models or Path(args.mfa).resolve().parents[2] / "models")
    # MFA writes its database through the sqlite3 tool: given a relative
    # MFA_ROOT_DIR, it lost its tables after aligning (no such table:
    # word_interval_temp). Every path it is handed is absolute.
    args.work = args.work.resolve()
    models = models.resolve()

    package = args.project.parent
    cache = package / ".cutmachine"
    library = load_library(args.project)
    by_key = {}
    for media in library:
        by_key[media["id"]] = media
        by_key[Path(media["filename"]).stem] = media
    overrides = {}
    if args.text:
        for key, text in json.loads(args.text.read_text(encoding="utf-8")).items():
            overrides[by_key[key]["id"]] = text
    args.work.mkdir(parents=True, exist_ok=True)
    (args.work / "wav").mkdir(exist_ok=True)

    jobs = []
    report = {"aligned": [], "skipped": [], "uncovered_speech": []}
    for key in args.media:
        media = by_key.get(key)
        if media is None:
            print(f"inconnu : {key}", file=sys.stderr)
            return 2
        transcript = json.loads((cache / "transcripts" / f"{media['id']}.json")
                                .read_text(encoding="utf-8"))
        language, language_source = alignment_language(transcript)
        if language not in MODELS:
            print(f"{key} : pas de modèle MFA pour la langue « {language} », "
                  "ignoré", file=sys.stderr)
            report["skipped"].append({"media_id": media["id"],
                                      "reason": "unsupported_language",
                                      "language": language})
            continue
        rate = (media["rate"]["num"], media["rate"]["den"])
        duration = media["duration"]
        duration_frames = int(Fraction(duration["value"] * rate[0],
                                       duration["rate"] * rate[1]))
        tokens = []  # (surface text, acoustic words)
        times = []  # each token's Whisper (start, end) in seconds, or None
        items = ([{"text": overrides[media["id"]]}] if media["id"] in overrides
                 else transcript["words"])
        if media["id"] not in overrides:
            holes = uncovered_speech(cache, media, items, rate)
            if holes:
                print(f"{Path(media['filename']).stem} : parole sans texte "
                      f"{holes} (images source) — vérifie, puis --text",
                      file=sys.stderr)
                report["uncovered_speech"].append({
                    "media_id": media["id"],
                    "frames": [[begin, end] for begin, end in holes]})
        for item in items:
            surface = unicodedata.normalize("NFKC", item["text"]).strip()
            span = item_seconds(item)
            found = list(TOKEN.finditer(surface))
            for index, match in enumerate(found):
                begin = 0 if index == 0 else match.start()
                end = (found[index + 1].start() if index + 1 < len(found)
                       else len(surface))
                acoustic = spoken(match.group(), language)
                if acoustic:
                    tokens.append((surface[begin:end].strip(), acoustic))
                    times.append(span)
                elif tokens:
                    tokens[-1] = (tokens[-1][0] + " " + surface[begin:end]
                                  .strip(), tokens[-1][1])
        if not tokens:
            print(f"{key} : aucun mot alignable, ignoré", file=sys.stderr)
            report["skipped"].append({"media_id": media["id"],
                                      "reason": "no_alignable_word"})
            continue
        stem = Path(media["filename"]).stem
        # ITW3 -- the media id has no spaces and is unique: the WAV and MFA's
        # corpus files are named after it; the filename stays the sample_id.
        wav = args.work / "wav" / f"{media['id']}.wav"
        if not wav.exists():
            source = (package / media["path"]).resolve()
            subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel",
                            "error", "-i", str(source), "-map", "0:a:0", "-vn",
                            "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le",
                            "-y", str(wav)], check=True)
        text = " ".join(w for _, acoustic in tokens for w in acoustic)
        jobs.append({"media": media, "stem": stem, "rate": rate,
                     "duration_frames": duration_frames, "tokens": tokens,
                     "times": times, "text": text, "transcript": transcript,
                     "wav": wav, "language": language,
                     "language_source": language_source,
                     "duration_seconds": float(Fraction(duration["value"],
                                                        duration["rate"]))})

    if not jobs:
        # Nothing to hand MFA: starting it for an empty corpus costs seconds.
        if args.report:
            report["mfa_ms"] = 0
            args.report.write_text(json.dumps(report, ensure_ascii=False) +
                                   "\n", encoding="utf-8")
        return 0
    missing = [str(models / MODELS[language][kind])
               for language in dict.fromkeys(job["language"] for job in jobs)
               for kind in ("acoustic", "dictionary", "g2p")
               if not (models / MODELS[language][kind]).is_file()]
    if missing:
        print("modèles MFA introuvables : " + ", ".join(missing),
              file=sys.stderr)
        return 2
    version = mfa_version(args.mfa, args.work / "mfa-root")
    # ITW9 -- one MFA run per language, each with its own models, in the
    # order the languages first appear among the rushes.
    languages = list(dict.fromkeys(job["language"] for job in jobs))
    total_ms = 0
    for language in languages:
        group = [job for job in jobs if job["language"] == language]
        started = time.monotonic()
        output, unaligned = run_mfa(args.mfa, models, language, group,
                                    args.work, args.jobs, segmented=False)
        outputs = {job["media"]["id"]: output for job in group}
        retry = [job for job in group
                 if job["media"]["id"] in unaligned and can_segment(job)]
        if retry:
            second, still = run_mfa(args.mfa, models, language, retry,
                                    args.work, args.jobs, segmented=True)
            for job in retry:
                outputs[job["media"]["id"]] = second
                job["segmented"] = job["media"]["id"] not in still
            unaligned = (unaligned - {job["media"]["id"] for job in retry}
                         ) | still
        wall_ms = int((time.monotonic() - started) * 1000)
        total_ms += wall_ms
        for job in group:
            if job["media"]["id"] in unaligned:
                # MFA leaves a file it could not align out of its output and
                # without a likelihood, and exits 0.
                print(f"{job['stem']} : MFA n'a pas pu l'aligner, ignoré",
                      file=sys.stderr)
                report["skipped"].append({"media_id": job["media"]["id"],
                                          "reason": "not_aligned"})
                continue
            if not write_alignment(job, outputs[job["media"]["id"]], wall_ms,
                                   version, cache, overrides, report):
                report["skipped"].append({"media_id": job["media"]["id"],
                                          "reason": "not_aligned"})
    print(f"MFA : {total_ms / 1000:.1f} s pour {len(jobs)} rush(es)")
    if args.report:
        report["mfa_ms"] = total_ms
        args.report.write_text(json.dumps(report, ensure_ascii=False) + "\n",
                               encoding="utf-8")
    return 0


def mfa_version(mfa: str, root: Path) -> str:
    result = subprocess.run([mfa, "version"], capture_output=True, text=True,
                            env=mfa_environment(mfa, root), check=True)
    return "MFA " + result.stdout.strip().splitlines()[-1]


def mfa_environment(mfa: str, root: Path) -> dict:
    """MFA finds Kaldi's tools on its environment's PATH, and writes its
    working files to MFA_ROOT_DIR (~/Documents/MFA by default). Its Python
    must not read the user's own packages (~/.local): a click of another
    version there crashed a run after its first pass."""
    environment = dict(os.environ)
    environment["PYTHONNOUSERSITE"] = "1"
    environment["PATH"] = (str(Path(mfa).resolve().parent) + os.pathsep +
                           environment.get("PATH", ""))
    environment["MFA_ROOT_DIR"] = str(root)
    return environment


def can_segment(job: dict) -> bool:
    return (job["duration_seconds"] > WHOLE_RUSH_SECONDS and
            bool(job["times"]) and None not in job["times"])


def run_mfa(mfa: str, models: Path, language: str, group: list[dict],
            work: Path, jobs: int, segmented: bool) -> tuple[Path, set[str]]:
    """Aligns one language's rushes, each whole or cut at its silences;
    returns MFA's output directory and the media ids it left unaligned."""
    name = f"mfa-{language}" + ("-segmented" if segmented else "")
    corpus = work / name / "corpus"
    output = work / name / "output"
    shutil.rmtree(corpus.parent, ignore_errors=True)
    corpus.mkdir(parents=True)
    for job in group:
        target = corpus / job["wav"].name
        try:
            os.link(job["wav"], target)
        except OSError:
            shutil.copyfile(job["wav"], target)
        segments = (utterance_segments(job["duration_seconds"], job["times"],
                                       silences(job["wav"]))
                    if segmented else None)
        if segments is None:
            (corpus / f"{job['media']['id']}.lab").write_text(
                job["text"] + "\n", encoding="utf-8")
        else:
            write_textgrid(corpus / f"{job['media']['id']}.TextGrid",
                           job["duration_seconds"], segments, job["tokens"])
    files = MODELS[language]
    with open(work / f"{name}.log", "w") as log:
        subprocess.run([mfa, "align", str(corpus),
                        str(models / files["dictionary"]),
                        str(models / files["acoustic"]), str(output),
                        "--g2p_model_path", str(models / files["g2p"]),
                        "--output_format", "json", "--clean", "--final_clean",
                        "--single_speaker", "--num_jobs", str(jobs),
                        "--beam", MFA_BEAM, "--retry_beam", MFA_RETRY_BEAM],
                       check=True, stdout=log, stderr=subprocess.STDOUT,
                       env=mfa_environment(mfa, work / "mfa-root"))
    unaligned = set()
    with open(output / "alignment_analysis.csv", encoding="utf-8") as table:
        for row in csv.DictReader(table):
            if not row.get("overall_log_likelihood"):
                unaligned.add(row["file"])
    unaligned |= {job["media"]["id"] for job in group
                  if not (output / f"{job['media']['id']}.json").exists()}
    return output, unaligned


def item_seconds(item: dict) -> tuple[float, float] | None:
    if "start" not in item or "end" not in item:
        return None
    return (float(Fraction(item["start"]["value"], item["start"]["rate"])),
            float(Fraction(item["end"]["value"], item["end"]["rate"])))


def silences(wav: Path) -> list[tuple[float, float]]:
    """Runs of the 16 kHz WAV under the engine's voice line, in seconds, at
    least CUT_SILENCE_SECONDS long."""
    try:
        with wave.open(str(wav), "rb") as handle:
            rate = handle.getframerate()
            samples = array("h")
            samples.frombytes(handle.readframes(handle.getnframes()))
    except (EOFError, wave.Error):
        return []
    if sys.byteorder == "big":
        samples.byteswap()
    powers = [sum(value * value for value in samples[start:start +
                                                     VOICE_WINDOW_SAMPLES])
              for start in range(0, len(samples) - VOICE_WINDOW_SAMPLES + 1,
                                 VOICE_WINDOW_SAMPLES)]
    if not powers:
        return []
    ranked = sorted(powers)
    speech = ranked[max(0, -(-VOICE_PERCENTILE * len(ranked) // 100) - 1)]
    line = VOICE_LINE_RATIO * VOICE_LINE_RATIO * speech
    window = VOICE_WINDOW_SAMPLES / rate
    runs, start = [], None
    for index, power in enumerate(powers + [line + 1]):
        if power < line and start is None:
            start = index
        elif power >= line and start is not None:
            if (index - start) * window >= CUT_SILENCE_SECONDS:
                runs.append((start * window, index * window))
            start = None
    return runs


def utterance_segments(duration: float, times: list,
                       quiet: list[tuple[float, float]]) -> list | None:
    """The utterances a long rush is handed to MFA as: (start, end, token
    indices), cut in the middle of a silence once the utterance has lasted
    SEGMENT_SECONDS, never in a silence a Whisper word overlaps. None for a
    rush short enough to go whole, or whose words have no times (a text
    given with --text)."""
    if duration <= WHOLE_RUSH_SECONDS or not times or None in times:
        return None
    cuts, last = [], 0.0
    for start, end in quiet:
        middle = (start + end) / 2
        if middle - last < SEGMENT_SECONDS or duration - middle < 1.0:
            continue
        if any(word[0] < end and word[1] > start for word in times):
            continue
        cuts.append(middle)
        last = middle
    if not cuts:
        return None
    bounds = [0.0] + cuts + [duration]
    members: list[list[int]] = [[] for _ in cuts + [duration]]
    segment = 0
    for index, (start, end) in enumerate(times):
        # Never back into an earlier utterance: the text stays in order.
        while (segment + 1 < len(members)
               and (start + end) / 2 >= bounds[segment + 1]):
            segment += 1
        members[segment].append(index)
    return [(bounds[i], bounds[i + 1], members[i]) for i in range(len(members))]


def write_textgrid(path: Path, duration: float, segments: list,
                   tokens: list) -> None:
    """A long-format TextGrid with one tier: each utterance's spoken words,
    and empty intervals where an utterance has none."""
    intervals = []
    for start, end, members in segments:
        text = " ".join(word for index in members for word in tokens[index][1])
        intervals.append((start, end, text.replace('"', '""')))
    lines = ['File type = "ooTextFile"', 'Object class = "TextGrid"', "",
             "xmin = 0", f"xmax = {duration}", "tiers? <exists>", "size = 1",
             "item []:", "    item [1]:", '        class = "IntervalTier"',
             '        name = "speech"', "        xmin = 0",
             f"        xmax = {duration}",
             f"        intervals: size = {len(intervals)}"]
    for number, (start, end, text) in enumerate(intervals, start=1):
        lines += [f"        intervals [{number}]:", f"            xmin = {start}",
                  f"            xmax = {end}", f'            text = "{text}"']
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def letters(word: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFC", word.casefold())
                   if c.isalnum())


def token_spans(tokens: list, words: list) -> list | None:
    """Each token's (surface, start, end) from MFA's words.

    MFA may split what it was given ("centre-ville" comes back as two words)
    or keep it whole, so the two are matched letter by letter: a token spans
    the MFA words its letters landed on. A token none of whose letters
    matched joins the previous one, as a word left without a frame does.
    None when fewer than 95 % of the letters match: the output is not the
    text that was given."""
    expected, owners = [], []
    for index, (_, acoustic) in enumerate(tokens):
        for word in acoustic:
            for character in letters(word):
                expected.append(character)
                owners.append(index)
    observed, sources = [], []
    for index, (text, _, _) in enumerate(words):
        for character in letters(text):
            observed.append(character)
            sources.append(index)
    matcher = difflib.SequenceMatcher(None, "".join(expected),
                                      "".join(observed), autojunk=False)
    landed: dict[int, list[int]] = {}
    matched = 0
    for block in matcher.get_matching_blocks():
        for offset in range(block.size):
            landed.setdefault(owners[block.a + offset], []).append(
                sources[block.b + offset])
            matched += 1
    if not expected or matched < 0.95 * len(expected):
        return None
    entries = []
    for index, (surface, _) in enumerate(tokens):
        hits = landed.get(index)
        if not hits:
            if entries:
                previous = entries[-1]
                entries[-1] = (previous[0] + " " + surface, previous[1],
                               previous[2])
            continue
        start = min(words[i][1] for i in hits)
        end = max(words[i][2] for i in hits)
        if entries and start < entries[-1][1]:
            start = entries[-1][1]
        entries.append((surface, start, max(start, end)))
    return entries


def write_alignment(job: dict, output: Path, wall_ms: int, version: str,
                    cache: Path, overrides: dict, report: dict) -> bool:
    """Reads MFA's words for one rush and installs its alignment cache."""
    media, stem, rate = job["media"], job["stem"], job["rate"]
    tokens, text, language = job["tokens"], job["text"], job["language"]
    model = MODELS[language]["name"]
    tiers = json.loads((output / f"{media['id']}.json")
                       .read_text(encoding="utf-8"))["tiers"]
    words = [(label, Decimal(str(start)), Decimal(str(end)))
             for start, end, label in tiers["words"]["entries"]
             if label and label not in ("<eps>", "<unk>", "spn", "sil")]
    entries = token_spans(tokens, words)
    if entries is None:
        print(f"{stem} : les mots de MFA ne recouvrent pas le texte donné "
              f"({len(words)} mots MFA pour {len(tokens)} jetons)",
              file=sys.stderr)
        return False
    words = quantize(entries, rate, job["duration_frames"], language)
    sha = hashlib.sha256(" ".join(text.split()).encode("utf-8")).hexdigest()
    result = {
        "schema_version": 1,
        "sample_id": stem,
        "media_id": media["id"],
        "language": language,
        "source_rate": {"num": rate[0], "den": rate[1]},
        "alignment": {
            "method": "mfa",
            "version": version,
            "model": model,
            "timing_provenance": "mfa-word-tier",
            "reference_text_sha256": sha,
        },
        "processing": {"wall_time_ms": wall_ms,
                       "audio_duration_frames": job["duration_frames"]},
        "reference": {
            "whisper_model": job["transcript"].get("whisper_model"),
            "verbatim": job["transcript"].get("verbatim"),
            "text_override": media["id"] in overrides,
        },
        "words": words,
    }
    target = cache / "word-alignments" / media["id"]
    target.mkdir(parents=True, exist_ok=True)
    path = target / f"mfa-{model}-{sha[:16]}.json"
    path.write_text(json.dumps(result, ensure_ascii=False, indent=1) + "\n",
                    encoding="utf-8")
    print(f"{stem}: {len(words)} mots -> {path}")
    report["aligned"].append({"media_id": media["id"], "words": len(words),
                              "segmented": job.get("segmented", False),
                              "path": str(path), "language": language,
                              "language_source": job["language_source"],
                              "model": model})
    return True


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
