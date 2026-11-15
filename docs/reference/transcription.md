# Verbatim transcription and hesitation cleanup

## Configuring the model

The path to the Whisper model is a **local setting of the machine**, never
project data: a path saved in the project would not exist on another Mac, and
the same edit must open everywhere. It is set once, in
`~/.config/cutmachine/.env` — the same file that can carry
`ANTHROPIC_API_KEY`, the chat panel's fallback key:

```sh
echo 'CUTMACHINE_WHISPER_MODEL=/chemin/vers/ggml-large-v3.bin' \
  >> ~/.config/cutmachine/.env
```

The environment variable takes precedence over the file, so a one-off trial is
done by prefixing it to the command, without modifying anything.

Once set, the setting serves identically `--transcribe` without an explicit
path, the MCP tool `transcribe_media`, including when the application's Agent
panel calls it, and the window's transcription menus. As long as it is not set,
the tools refuse in the same way, naming the variable and the file to edit; the
window's menus ask for a file through a dialog. **CUTMACHINE → Modèle
Whisper…** (Whisper model…) and this dialog write the chosen model into
`~/.config/cutmachine/.env`, like `CUTMACHINE_WHISPER_MODEL`: the rest of the
file is kept as is, and a new file is readable only by its owner. A variable
exported in the application's environment keeps priority, and the status line
says so.

There is only one setting (DEC23,
LIVRAISON-2026-09-18). The window's menus
do not keep a model of their own in the macOS preferences
(`CUTMACHINEWhisperModelPath`): at first launch, a model chosen there by an
earlier version is copied into the file if the file does not yet designate any
model, then the preference is erased; if the file already designates one, it is
the one that stays.

`transcribe_media` is what makes work on words reachable by an agent:
`list_disfluencies`, `remove_words` and interview editing all read a
transcript, and none of them could bring it into existence. `verbatim: true` is
needed to be able to remove hesitations afterwards — the default decoding
silently suppresses them, so they are no longer there to be cut.

## Experimental French hesitation profile

`fr-hesitations-v1` selects the decoding profile evaluated with the s075
weights (AL4). It requires `language: "fr"` and `verbatim: true`. Before
inference, the engine verifies the weights' SHA-256:
`5b05a0870a122a833e4c74fbc223e1acbb74875bab3e3682d9cef692e51e385e`.
Other weights are refused even if renamed. The engine does not download
weights or replace the shared local model setting automatically.

In the application, **Transcrire les hésitations (français)…** (Transcribe
hesitations, French…) in a media item's context menu or the Timeline menu
selects this profile and French verbatim decoding. First choose the s075
weights in **CUTMACHINE → Modèle Whisper…** (Whisper model…). The existing
transcription commands keep the `default` decoding policy.

```sh
./build/cutmachine --transcribe ./Film.cutmachine-project/project.cutmachine.json \
  '<media-id>' /path/to/ggml-large-v3-hesitations-s075.bin fr \
  --verbatim --decoding-profile fr-hesitations-v1
```

The MCP tools `transcribe_media` and `transcribe_timeline` accept
`decoding_profile: "fr-hesitations-v1"` with `language: "fr"` and
`verbatim: true`. The CLI also accepts `--decoding-profile` for
`--transcribe-timeline` and `--srt-from-media`.

The cache records `decoding_profile`; an absent field means `default`.
Caches from different profiles never match. Timeline transcription caches
use separate files. For a media item, the new transcript replaces the usual
cache file under the existing safeguards (`cache_kept` and `overwrite`);
the reading tools report the stored transcript's actual profile. A plain
fallback pass uses `default`; a merged transcript retains the verbatim
profile's provenance together with `plain_pass_words`.

This profile is experimental. Its development controls do not establish
precision or recall on unseen recordings. The measurements and human checks
are recorded in AL4.
MFA alignment and the decision to cut a hesitation remain separate steps.

## Reading and editing words

`list_source_words` (AGENT-2026-09 A4) reads a source clip's words as the
aligner placed them (`word_timing` says who: `mfa`, `nemo` for a source clip
aligned by NeMo, or `whisper`) and, for each, the frame to come in on and the
one to go out on, read on the source clip's envelope by the interview skill's
rules: a start in silence takes the attack minus two frames of air, a voiced
start steps back to the silence, a voiced end moves forward to the drop-off
without passing the next word, an already silent end steps back to the last
sound plus three frames; `entry_rule` and `exit_rule` name the rule,
`voiced: false` a word that the envelope does not hear. `find` looks up a word
and returns its neighbours, `from_frame` and `to_frame` bound, `max_words`
caps. Without an envelope (`analyze_speech_onset`), the frames are the
aligner's. The three per-source-clip analyses — `analyze_speech_onset`,
`analyze_shot_quality`, `analyze_hesitations` — take `media_ids` to analyse all
the source clips in one call, one line per source clip under `results` (A8).
With this envelope in cache, `remove_words` and `clean_disfluencies` cut each
word from its entry to its drop by the same rules, and say so (`boundaries`,
`widened_frames`); `trim_boundary_air` and `close_junction_air` read the same
voice line as `tighten_pauses`; `ripple_trim` reports under `repeats_source`
the frames that a lengthening makes play twice, when the neighbouring clip
already plays them (A9 to A12). `propose_exposure` (A16) measures all the
picture clips of a timeline under colour management and returns, for each one
whose scene median differs by more than `tolerance_stops` from mid grey
(`target_stops`), a `set_clip_effects` step with its own corrected exposure, to
be passed to `prepare_edit_plan` or applied one by one; nothing is applied by
the tool, and `summary` says where the edit stands. `propose_grade` (A20 to
A22) reads each clip where the colourist reads it: the largest face the engine
finds (Vision), or a named white or grey square in `region`, or the whole
frame; it proposes the anchor's exposure toward `target_stops` (0.7 for a face)
and its balance on the skin line or at neutral, with the clip's temperature and
tint, and stops a lift where the frame's p98 would pass diffuse white
(`highlight_ceiling_stops`); a clip with no face follows the edit.
`list_timeline_words` (A24) projects the source clips' alignments onto each
audio clip of the edit, without transcribing it: the words and their frames, a
word cut at the entry or the exit, the air at the boundaries, the sound outside
words and the edit's text in one call; `list_source_words` with `levels` and a
range returns the whole envelope of the range (A23).
`describe` takes `only` (sections) and `kinds` (track kinds) to return only
what is asked of it (A25).
`tighten_pauses` and `trim_boundary_air` take `clip_ids`, `close_junction_air`
`pairs`: one call for all the clips, one result per clip (A26).
`transcribe_media` returns the text of each transcribed source clip (A27).
`create_project` creates a missing parent (A28); a refused speech selection
names all the faulty segments at once (A29).
`tighten_pauses` protects from an aligned word only its audible windows, and
closes the dip that a late alignment end held open (A30).
`propose_grade` grades per source clip: the anchored clips vote, all the
clips cut from that source clip receive the vote (A31).
A hesitation's suggested cut never nicks a neighbouring aligned word (A33).
`generate_captions` lengthens or merges a caption that is too short and closes
a gap between two captions (A32); a caption that is too short never merges
over a sentence end, and the one that opens the film or a sentence takes the
capital (REGLES-2026-10 RM2); `ripple_trim` carries the sync-locked tracks by
default (A34); `list_disfluencies` lists discourse markers (A35);
`trim_boundary_air` returns `ok` and `analyze_hesitations` also counts in
frames (A36).
`range_levels` also gives the envelope at its 20 ms windows (A37); a caption
check with no reading of the edited sound says so (A38).
`propose_grade` trims a face box that overflows and does not fail on an
unreadable clip (A39); `transcribe_media` returns its text even without an
envelope (A40).
`propose_grade` takes `apply` and `verbose` (A41); the last caption stops with
the sequence (A42); `insert_clip` does not move the other tracks, `ripple_trim`
does (A34).
`list_disfluencies` returns restarts, a word said twice that the transcription
wrote once (A43); `list_timeline_words` counts the hesitations and restarts
that each clip still plays (A44).
`cut_words` reports an aligned word on a frame against a cut (A45).
Each audio edge without its own fade receives a 3 ms fade, at export as at
playback: no click at any join (A46).
A hesitation cut keeps two frames of a word or syllable, not four, and a
restart takes along the "euh" that precedes it (A47).
A hesitation's cut runs to the end of the voice that follows the hold, a hold
between two words counts from 100 ms, a word placed in a hold is named, a clip
entered or exited in continuing voice says so (`head_in_voice`,
`tail_in_voice`), and a cut never enters an aligned word (A48).
`tighten_pauses` reports an aligned word that the envelope does not hear
(`heard: false`, peak and floor published) and, with `close_unheard`, closes
the dip over it (A17); `list_source_words` returns under `voice_without_words`
the voiced ranges that no word covers (A18).

## The rest

`--transcribe` accepts an explicit language and a `--verbatim` mode. The model
path remains optional there: if given, it takes precedence; if omitted, the
local setting applies.

It also accepts **several `media-id`s separated by commas** (MCP: `media_ids`),
and then loads the model only once. Loading takes ~8 s; on the 43 spoken source
clips of a real project, that was nearly six minutes spent rereading the same
file, against 11 min of useful inference. Media that the document says are
silent are **skipped and reported** rather than transcribed — 29 of the same
project's 71 source clips were cutaways at −74 dBFS, all passed to Whisper at
11× real time to produce nothing. `--include-silent` (MCP: `include_silent`)
forces the pass.

This level comes from `--ingest`, which measures the average audio level of
each media item and records it in the document (`audio_level`), just like the
frame rate and the duration. It is an audio decode, hence a real cost: measured
at 0.3 s on a 268 MB 4K source clip. A library entry written before this field
is read back as "not measured" — never "silent"; a repeated `--ingest` fills
it.

Automatic language detection is wrong on a long, mostly non-spoken source clip
(measured: Welsh detected on a French interview). Name the language.

Without `--verbatim`, Whisper cleans up what it hears: on a 7 min 35 interview
it kept only 3 tics out of the 20 actually spoken. Verbatim mode places in
front of each 30 s window a prompt that biases the decoding toward hesitations,
repetitions and false starts — hence 607 words transcribed instead of 390, and
the "euh", "heu", "ben" and "bah" present in the cache. The flag is part of the
transcript's identity: a verbatim transcription is never reused in place of a
standard one, nor the reverse.

Verbatim mode can also empty a take: on source clips in English and Spanish, 2
to 6 words for 13 to 22 s of voice, the crew's words at the head and tail, the
sentence skipped (ITW9). When the speech envelope signals it
(`likely_incomplete`), `--transcribe` and `transcribe_media` redo these media
without the prompt; the ordinary pass becomes the transcription, with only the
hesitations of the verbatim pass that fall in its silences. The cache stays
verbatim and says `plain_pass_words`: those words carry no hesitation, which
only signal analysis then finds (see "The hesitations nobody transcribed").
The detail is in [Commandes headless](commandes-headless.md).

What empties a take is an ellipsis: whisper answers "..." alone, with
confidence, over speech it hears badly (a distant microphone, a room), and the
segment carries the decoding past that speech. The ordinary pass does it too;
the verbatim pass more often, because its prompt ends in "..." (ITW9, measured
on 15 November 2026). When the "..." words of a transcription cover at least
2 s of measured speech, `--transcribe` and `transcribe_media` redo the pass
that gave the words with that token forbidden, and put its words in place of
each ellipsis laid over at least half a second of speech. An ellipsis whose
words are a known subtitle credit, a looping phrase or too few words for the
speech it covers stays as it is: that is what whisper says when it may not say
"..." and hears nothing it can decode. The cache says `ellipsis_pass_words`,
the words that came from that pass. Each media item is decoded on a whisper
state of its own: the same clip returns the same words, whatever batch it is
in.

Cleanup is then done in two steps, and never by a model:

1. `--disfluencies` (MCP: `list_disfluencies`) lists what is *provable* — the
   syllables that are not French words, and the immediately repeated words — as
   word indexes, with their text for review. What is a matter of judgment ("is
   this 'donc' a tic?") is listed apart, as a discourse marker (`Marker`, A35),
   and never cut automatically.
2. `--remove-words` (MCP: `remove_words`) applies the cut. It takes only **word
   indexes**: `ResolveWordRemoval` computes the frames, never the caller. The
   cut is a single, reversible `RemoveWordsOperation`, which closes the
   timeline up.

A model can therefore propose a selection and justify it, without ever
computing a timecode.

`clean_disfluencies` (MCP) brings the two steps together in a single intent:
CUTMACHINE detects, computes the frames and applies **one** reversible
operation for the whole clip. The model enumerates nothing. Repetitions are
removed only if `include_repetitions` is requested — a repeated word can be an
intended emphasis, a hesitation syllable never.

## Choosing passages: spans, blocks, selection

> IA-2026-09 IA3.

`get_timeline_transcript` (MCP) reads the cached transcript of the audible
clips as spans, each named by a `span_id`, and returns a `transcript_revision`.
The identifiers are positions in this reading: a tool that receives them with
an older revision refuses them with `StaleSelection`. The exact times are given
for a human to check, never to be copied into another tool. A span carries
`straddles_cut` when it contains a word cut at a join, `likely_hallucinated`
when words exist with no measured speech group (the selection tools refuse it
without `force: true`), and `likely_incomplete` when sustained speech produced
too few words: the missing text there is not a silence.

`list_speech_blocks` groups these spans into speech blocks, cut where a
sentence ends, where the person pauses longer than a breath, or where the
source clip changes: this is where to choose ideas, rather than in subtitle
captions. `opens_mid_sentence` and `closes_mid_sentence` flag a block that is
not a whole sentence (a false flag is omitted). `inspect_speech_selection`
checks a selection before building it, changing nothing: for each segment, its
range and its text, whether it opens or closes mid-sentence, what the same
source clip says just before and just after, with `kept` (the selection keeps
it elsewhere) and `heard_next_to_it` (the neighbouring segment plays it), and
`overlaps_segments` for segments that replay the same material. The engine
reports; judging the sense is up to the caller.

`create_interview_short` creates a timeline from spans or blocks named with the
`transcript_revision` read at the same time, and activates it, except with
`activate: false` or in strict mode; the original timeline does not change. The
engine resolves each identifier to an exact source range, which keeps a cut out
of a word.

## Realigning words on the signal, and closing silences

`--speech-onset` also caches the speech groups separated by at least 200 ms of
gap. `--speech-onset-report` (MCP: `list_speech_onsets`) publishes for each
group its source bounds, its average level and its peak in dBFS. The
`dominant_onset` field designates the first group held for at least six 20 ms
windows and located within 9 dB of the clip's 90th percentile. A weak
off-microphone question therefore stays visible in `groups`, but is not
confused with the entry of the subject wearing a lavalier microphone. The
threshold separating the groups and their floor above the noise are
configurable in the MCP tools (`group_gap_ms`, `group_floor_db`).

Whisper's timestamps are excellent most of the time and occasionally wrong by a
second. `--align-transcripts` (MCP: `align_transcript`) compares each word
boundary with the energy envelope produced by `--speech-onset`, corrects those
that fall in silence, and **refuses** the others rather than moving them by
guesswork — a boundary that is ambiguous, has no speech onset in range, or
would require a shift beyond the ceiling is reported and left where it is.
Without `--write`, it is a report; with it, the correction is written into the
transcription cache that `remove_words`, `clean_disfluencies` and interview
editing read. This is what makes a per-word cut reliable; without it, the only
way to check a boundary was to extract a fragment and transcribe it again.

`--tighten-pauses` (MCP: `tighten_pauses`) closes a clip's **internal**
silences without reading a single word: it looks for envelope gaps of at least
`min_gap_ms` milliseconds (320 by default), brings each down to `keep_frames`
frames (4 by default, split between the two edges, so that the join falls in
the deepest part of the silence), and closes up with a ripple. These defaults
are the setting that the interview procedure passed explicitly; they replace
400 ms and 6 frames, which left the film soft. Under 300 ms, a gap inside a
word (a stop consonant) has already been closed. A single
`RemoveWordsOperation`, hence a single `undo`, and the linked A/V pair is taken
along with it. The head and tail air is not touched: that is trimming, the
question of `--speech-onset`, and it is published in the report rather than cut
on the sly.

`--trim-boundary-air` (MCP: `trim_boundary_air`) closes precisely these head
and tail silences. The two optional numbers are, in order, the source frames
kept next to the speech (3 by default) and the minimum duration of air handled
in milliseconds (300 by default). The bounds come from the speech groups
measured by `--speech-onset`, never from Whisper's words. The A/V pair and the
other sync-locked tracks follow in a single `TrimBoundaryAirOperation`, hence a
single `undo`. The pair is the single partner of each trimmed edge: on each
other track, the linked-group member whose same edge is closest, as for
`ripple_trim`. The other pieces of a block already cut up (by
`remove_source_ranges` or `--tighten-pauses`, which leave them a single group)
do not move.

At a cut, use `--close-junction-air` (MCP: `close_junction_air`) with the
outgoing clip then the incoming clip. The engine adds the first one's tail air
and the second one's head air before comparing the threshold, then trims both
sides atomically. Thus two 280 ms halves are properly recognised as an audible
560 ms gap, and a second pass does not move the junction again. The MCP tools
additionally accept `sync_track_ids` to explicitly replace the deduced
sync-lock tracks.

When a forced word alignment is in cache, `--tighten-pauses` never closes a gap
that overlaps an aligned word: the gap is cut around the word and each piece is
judged as a gap in its own right. A one-frame "à" left below the speech line
between two short silences is not swallowed with them. Without alignment, the
closing is unchanged and the report says so (`words_checked: false`); the
protected words are named in `kept_words`.

Any edit that removes sound from the timeline names the words it takes along.
The result of `--apply-op`, of the cutting commands and of the MCP tools
`tighten_pauses`, `trim_boundary_air`, `close_junction_air`,
`remove_source_ranges`, `ripple_trim`, `remove_words` and `clean_disfluencies`
carries `cut_words`: per source, the frames removed and the aligned words
touched, whole or nicked (`whole: false`), split between `targeted` (the words
requested of `remove_words`, the hesitations of `clean_disfluencies`) and
`carried` (all the others). A source without forced alignment is declared
`checked: false` with the reason, rather than returned as an empty list. The
material removed is read on the audio tracks: trimming the picture alone
removes no heard word.

## Aligned boundaries and unheard words

Whisper provides the text, a forced aligner provides the boundaries: this is
the conclusion of the alignment bench. The
aligner is Montreal Forced Aligner (MFA 3.4.3) (ALIGNEMENT-2026-11): against
C-PROM, 70 minutes of hand-verified French, its boundaries are 31 ms from the
reference on average, those of NeMo Forced Aligner, which it replaces, 73 ms,
and those of Whisper nearly 300 ms. The engine reads this result
itself. An alignment is stored under
`.cutmachine/word-alignments/<media-id>/` in the bench's format (manifest in
version 1, words in source frames `[frame_start, frame_end)`); the `align_words`
tool (MCP) and `--align-words` (CLI) write it there, by launching
`scripts/mfa_align.py` with the Python of MFA's environment under the project
lock (see [Where MFA is looked up](lancement.md#where-mfa-is-searched)).
Each source clip is first aligned in one piece; a source clip of more than 90 s
that MFA leaves without alignment is retried cut up in the middle of its
silences (`segmented: true` in the response), and a source clip that it still
does not align is set aside (`not_aligned`) and keeps its earlier words. The
caches written by NeMo are still read.
Each source clip is aligned in the language of its transcription, with the MFA
models of that language (`french_mfa`, `english_mfa`, `spanish_mfa`: acoustic
model, pronunciation dictionary and G2P for words absent from the dictionary):
the language given to `transcribe_media` when it was forced, otherwise the one
that whisper detected (`detected_language`, kept in the cache (ITW9) and
returned by `transcribe_media`), otherwise the one that the words of the text
designate, French when nothing decides. Digits are written out in words in that
language. A source clip in another language is set aside
(`unsupported_language`). For a shoot in a language that detection confuses,
transcribe with the language forced: it is also the one that is aligned.
`remove_words`, `clean_disfluencies`, `list_disfluencies`,
`get_timeline_transcript`, `create_interview_short` and `--export-srt` all go
through the same reader: the most recent alignment replaces Whisper's words as
long as their letters agree. A transcription redone after the alignment
therefore falls back to Whisper rather than keeping stale boundaries. Each
response says who placed the boundaries (`word_timing`: `whisper`, `mfa`,
`nemo`…).

A word can be placed in the edit by the source clip's transcription without
being heard in the edited sound. `transcribe_timeline` keeps side by side a
standard reading and a verbatim reading of the edit, as long as they match its
audible arrangement. A word that none of these readings contains within two
seconds is set aside from the spans and the SRT, and listed in `unheard_words`
with the number of readings consulted (`montage_readings`). Without a reading,
nothing is set aside: the absence of proof removes no word.

## The hesitations nobody transcribed

Whisper, even in verbatim, does not write all the hesitations: on a real
interview, it transcribed none of the fourteen that the final edit cut. They
fall into three families: a detached "euh", a stretched syllable ("la
maa…rée") and a murmur between two words.

`analyze_hesitations` (MCP) looks for them in a source clip's sound, without
reading the words. The measure is **timbre stability**: the mean distance
between successive MFCC vectors over ±50 ms. Speech changes its spectral
envelope every few tens of milliseconds, a held vowel hardly at all. A portion
is held when its stability falls under 76 % of the median of the source clip's
voiced windows. The threshold is relative, because the absolute scale depends
on the analysis, whereas the ratio has been conserved from one measurement to
the next. A hold of less than 200 ms at the end of a word is the prosody of a
comma and is not listed; a murmur lies between 22 and 36 dB below the source
clip's speech level. Each candidate is placed against the aligned words, with
its family, its source range, its measure and the neighbouring words. The
result is cached in `.cutmachine/hesitations/<media-id>.json`.

`list_disfluencies` then adds to it, for the clip, a `hesitations` array
(`hesitation_analysis` is `missing` as long as the source clip is not
analysed): range in source frames, measures, words around and `suggested_cut`.
For a stretched syllable, the proposed cut leaves the first two frames of the
hold, so that the syllable is still heard (A47).
`remove_source_ranges` applies this cut: ranges in the source clip's frames,
`[first_frame, end_frame)`, a single reversible operation that takes along the
linked pair and the sync-lock tracks.

Cross-checked on the four source clips of a real edit: 20 candidates, 15 of
them in the used portion and 11 in what the editor finally cut. The other four
are either false alarms or kept hesitations: only listening tells them apart.
It is a measurement, not a cutting decision.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_hesitations '{"media_id":"<media-id>"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  remove_source_ranges \
  '{"clip_id":"<clip-id>","ranges":[{"first_frame":1412,"end_frame":1427}]}'
```

## Who is speaking: a source clip's voices

`analyze_speakers {media_id, speakers}` says who speaks when in a source clip
(PARITE-2026-11 PA3). `speakers` is the number of voices in the source clip,
from 1 to 8: you know it, the model does not guess it. Guessed, it was wrong on
five dialogues out of fifteen.

A voiceprint model (WeSpeaker ResNet34-LM, launched by sherpa-onnx, both under
Apache 2.0) reads the sound on the machine: nothing leaves it. It gives a
vector to each 1.5 s window of speech, one window every 0.25 s, and the vectors
are sorted into as many groups as there are voices. Transcribe the source clip
first: the speech passages are then its words, and a window never crosses from
one passage to the next (`speech_from: "words"`). Without a transcription, the
passages are those where the sound is loud (`"loudness"`).

The result is a cache, `.cutmachine/speakers/<media-id>.json`, not a state of
the document. Two readings:

- `list_speakers {media_id}` returns, per voice, its duration and its number of
  turns, then the turns in order: the voice, the times in the source clip, the
  number of words and the first twelve. A long list is paginated
  (`max_turns`, `from_turn`, `next_from_turn`).
- `list_source_words` adds `speaker` to each word: the voice that speaks over
  the largest share of the word.

A voice is a number, 0, 1, 2 in order of appearance, specific to its source
clip: nothing says who it is, and voice 0 of one source clip is not that of
another. You name it by reading the words.

`similarity` says how much two voices resemble each other, in thousandths.
Well-separated voices measured from 380 to 800; a single voice requested as two
measured 877, and the pairs that the model confused from 850 to 900. From 850,
the split is not reliable: either it is a single voice cut in two, or two
voices that it does not tell apart. The tool returns the number, not a verdict.

Measured on fifteen dialogues made with two real French voices each, all turns
known (`tools/speaker-diarization-benchmark/bench.py`):

| | share of time given to the wrong voice |
|---|---|
| on average | 5.9 % |
| at the median | 4.2 % |
| the worst pair (two very close voices) | 25.5 % |
| three voices taking turns | 4.1 % |

A change of voice is found to within 0.25 s, and a second of sound is read in
0.06 s. The usual sherpa-onnx chain (pyannote 3.0 segmentation then clustering),
tried first, gave 26 % on average on the same dialogues: it separated a man from
a woman and almost always missed the transition between two women in the same
room.

What the measurement does not say: these dialogues are turns glued end to end,
with no pause or overlap, and no real two-voice source clip whose answer is
known has been read. Two people speaking at once are not handled: a passage has
one voice.

For a multicam edit cut by voice, cut the group at the turns' times
(`split_multicam_group`) and give each piece its angle
(`set_multicam_active_angle`): the tool measures, the choice of the angle is
yours. Where the model is looked up:
[launch](lancement.md#where-diarization-is-searched).

## Addressing the timeline by source clip

An editing decision is made in the source clip ("cut just after the name",
"this take starts at 1412"), but all the operations take a **timeline**
position. The conversion is a subtraction and an addition — that is, exactly
what one gets wrong one time in ten.

`--locate-source-frame` (MCP: `locate_source_frame`) answers "where does frame
N of this source clip play": which clip, which track, which position. The same
frame can be on the timeline several times — the picture and its detached
sound, or a source clip used twice — so all the matches are returned, never
one chosen.

And the cutting and trimming tools accept the same address: `split_clip`,
`trim_clip` and `ripple_trim` take `source_frame` **in place of**
`timeline_position` or `delta` — never both, explicit refusal otherwise. On a
trim, `Head` means "the clip starts on this frame" and `Tail` "this is the last
frame played": the bound is inclusive, because that is what a frame number
means to an editor, and the +1 belongs to the engine.

A linked A/V pair is cut on both sides in the **same** operation.
`RemoveWordsOperation` carries a `linked_clip_ids` field: each named clip loses
exactly the same source frames and closes up its own track. Without that,
cleaning an interview's sound would shorten the audio and leave the picture,
and a single `undo` would no longer be enough to go back. The
`clean_disfluencies` and `remove_words` tools themselves resolve the members of
the link group that actually cover the cut; the operation, for its part, stays
strict — an explicitly named clip that does not contain the ranges is refused
(`SourceOutOfBounds`), never cut by half.
