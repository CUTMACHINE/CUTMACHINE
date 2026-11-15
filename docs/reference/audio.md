# Audio

> The mouse gestures and what one hears are not verified by reading; the
> colours are those of the tokens in `src/UiTheme.h`.

The audio streams of the sources are decoded by FFmpeg, then converted to
16-bit stereo 48 kHz PCM by `libswresample`. An `AVAudioSourceNode` mixes in
real time only the clips placed on audio tracks. Video clips are always
silent: no playback or export fallback picks up their audio. An audio clip can
carry an exact gain in dB and exact fade-in/fade-out:
`SetClipAudioOperation` atomically replaces `gain_db`, `fade_in` and
`fade_out`. Both durations are `RationalTime` values; the same envelope is
applied by playback and by the FFmpeg export. Document schema v7 serializes
them for each clip. Documents v4 to v6 remain readable: they unambiguously
receive 0 dB and two zero-length fades, then are written back as v7.
The audio callback reads an immutable plan built from the timeline and never
touches the editable document directly. `Espace` and `J/K/L` drive picture and
sound together, including the accelerated forward/reverse shuttle at 2× and 4×
(accelerated sampling, no pitch correction); a seek, a trim, a move, a gap
closing or an undo rebuilds the mix plan at the exact junction.
Summed samples are clamped to `[-1, 1]` to avoid numeric overflow when mixing
several tracks.

When an old project is opened, every video clip whose migration flag still
indicates embedded sound is split. The application creates audio tracks as
needed and emits one `DetachAudioOperation` per clip in the event log: the
audio rectangle receives its own ULID but keeps exactly the same `source_in`,
duration and `timeline_in`. All new edits create this pair directly, with
`include_audio:false` on the video, so the sound is never embedded nor played
twice. Audio and picture can then be moved, trimmed, cut or deleted
independently. This normalization is persisted and can still be undone with
`Cmd+Z`. The window has no button or shortcut to split a clip by hand: the MCP
tool `detach_audio` remains available to migrate an old clip manually.

Video rectangles use a grey palette, audio rectangles a green one, even when
they come from the same media. Each pair produced by a split shares a stable
`link_group_id` and an exact rational phase reference. With **Sélection liée**
(Linked Selection) on, clicking, lassoing or editing one member acts on the
pair; a drag shows both previews and emits a single atomic operation:
`MoveLinkedClipsOperation` or `TrimLinkedClipsOperation`; deleting the pair is
a `ClearClipsOperation`. With the toggle off, each rectangle stays
independent. `Cmd+Shift+L` switches the mode and `Option` suspends it for one
gesture. An independent audio move shows on the rectangle a badge signed in
frames (`+3f`) or, for a sub-frame offset, in samples (`-240smp`). Returning
to zero is magnetic. Old projects that were already split are migrated by an
atomic `SetClipLinkOperation` when the source and timeline times match
exactly.

Moving the playhead by hand produces an audio scrub: every click, every drag
in the ruler or in a gap, and every keyboard step triggers a 60 ms grain at the
requested position. A 5 ms envelope at the start and end prevents clicks; a
new movement immediately replaces the grain in progress without starting
continuous playback.
In Image mode (Frame mode), mouse events stay deduplicated after quantization:
as long as the cursor remains within the same frame, the grain is played only
once. It is rearmed only when entering another frame, which avoids the very
noisy restarts caused by sub-frame micro-movements.
In Image mode, the arrow keys move the playhead by one frame (`Shift`: ten);
in Échantillon mode (Sample mode), they move it by one sample (`Shift`: ten).
The bottom strip always shows the active grid.

## Measuring the offset between two sounds of the same take

`measure_audio_offset` (MCP; CLI `--measure-audio-offset`, SYNCHRO-2026-10
SY1) tells how far the sound of one source clip is offset from that of
another: a recorder's file against the camera sound, or two cameras on the
same take. It modifies nothing. `offset` is the position of the first sample
of `other_id` on the time of `reference_id`, exact to 48,000 samples per
second (negative when the other started earlier); `offset_frames` restates it
in frames of the reference, rounded to the nearest, for reading only. The
response also gives the overlap of the two sounds, a confidence
(`significance`: the peak above chance, in standard deviations; `peak_ratio`:
against the best peak elsewhere) and the drift of the two clocks, measured on
three windows (head, middle, tail). Each sound is read on the requested
channel (`mix`, `left`, `right`): a recorder often carries the lavalier on one
side and the boom on the other.

The measurement takes two passes: a coarse one, on the sound's onsets (where
it rises, which reverberation does not move) and over all offsets at once; a
fine one, to the sample, on the sound itself, phase-weighted (GCC-PHAT), which
keeps the arrival of the direct sound sharp in a reverberant room. It never
guesses: `NoOverlap` (less than 2 s in common), `WeakCorrelation` (peak less
than 10 deviations from chance: not the same take, or too little signal),
`AmbiguousPeak` (another offset fits almost as well: a sustained sound, a
loop, a tic), `ClockDrift` (more than half a frame of drift over the overlap,
at the frame rate of the reference: a single offset would be wrong at the end
of the take; no speed correction is applied).

Verified on synthetic signals (`audio_offset_tests`: exact to the sample
despite gain, noise, a filter and a room; the refusals) and on two files: a
synthetic voice placed 1 s earlier in a recorder WAV than in a camera MP4
(AAC) with reverberant sound, measured at −48,000 samples without a filter and
at −47,997 with a 3.5 kHz low-pass, whose own delay that is. The thresholds
(10 deviations, ratio 1.5) remain to be tested on real shoots.

### Laying the separate sound under the picture (SY2)

`sync_audio` (MCP; CLI `--sync-audio`) lays a recorder's sound (`media_id`)
under a picture clip (`clip_id`), on an audio track (`track_id`), in a single
undo step. The offset is measured between the clip's source clip and the
recorder as `measure_audio_offset` measures it (channels `picture_channel`,
`recorder_channel`), or taken from its response (`offset`); a refusal of the
measurement is the tool's refusal, under the same name. The sound covers the
part of the clip that the recorder contains, on timeline frames: a recorder
started or stopped during the clip is trimmed inward to the next frame, and
its in point keeps the measured sample. It is linked to the picture clip's
group with its exact phase: moving, trimming or cutting the picture takes it
along. The camera sound stays in place, linked, and its track is muted
(`mute_camera_sound`, true by default). The batch never overlaps what the
track already contains (`Overlap`). The response names the clip laid
(`placed`) and, when it measured, the measurement (`measure`). Nothing new in
the document: a paste, a link, a track output (`SeparateSound.h`).

With `sound_clip_id` in place of `media_id` and `track_id` (CLI
`--sync-audio-clip`), the recorder's sound is a clip already on the timeline,
as Resolve's Auto Align Clips takes it. It slides along its track until it is
aligned under the picture clip. It keeps its identifier, its gain and its
fades, and, of its own range, what falls under the clip, on timeline frames,
without ever reading more of its media than before. It is linked in the same
way, with the camera track muted, in one undo step. The batch never overlaps
another clip on the track (`Overlap`). A sound that is already aligned and
linked answers `unchanged`. Refused before any measurement: the clip's own
sound, a sound linked to another clip, a speed change, a locked track
(`CheckSeparateSoundPair`).

In the window, select the picture clip and the recorder's sound (Shift-click),
then right-click or use the Clip menu: « Aligner automatiquement les plans ›
Selon la forme d'onde » (Auto Align Clips › By waveform). A right-click on a
selected clip keeps the selection. The measurement runs in the background, and
the task bar shows it. A refusal is reported in the status bar, in French, and
nothing is laid. The gesture has not yet been done by hand.

Tried end to end on the synthetic voice: laid at 47,997/48,000 s of the
recorder under a 20 s clip, then the exported film (camera track muted, only
the laid sound audible) measured against the camera's source clip by
`measure_audio_offset`: offset 0, drift 0; `verify_export` reports it exact,
no divergent frame.

### Word cuts take the separate sound along (SY3)

A recorder sound linked to the picture clip (same link group, other source) is
cut with it by `remove_words`, `remove_source_ranges`, `clean_disfluencies`
and `tighten_pauses` (and their CLI commands): at the same instants, that is,
at the picture clip's ranges shifted by the exact difference of the two clips'
phases (`timeline_in − source_in`). The camera sound, of the same source,
loses the same source frames as before. The operation keeps the translated
ranges of each separate sound (`translated_links`), written only when there is
one: an earlier journal, or a cut without a separate sound, keeps its bytes.
The responses name them under `also_cut`. A separate sound that does not cover
all the ranges is not cut, like a member of the same source. A separate sound
on a locked track refuses the cut rather than come loose. Edge trims
(`ripple_trim`, `trim_boundary_air`, `close_junction_air`) already took the
nearest member of each track without looking at its source: the separate sound
loses the same head or the same tail there, and keeps its phase.

`create_interview_short` reads the timeline's links where the spans were read.
When the clip that holds a segment is linked to a recorder's sound, the film
keeps it: the recorder's sound on a third track, at the same instants (the
segment's range shifted by the exact difference of the phases), in the
segment's link group and anchored on its picture, so that `timeline_stats`
measures no drift there; the camera sound stays, linked, on its muted track,
as `sync_audio` leaves it. A segment read on the recorder's sound becomes the
picture's range, and the recorder's sound is kept alongside. The operation
writes the track and, for each segment, the source and in point of the
separate sound, only when there is one: an earlier journal keeps its bytes.
Refused rather than guessed: a segment that the recorder does not cover in
full (`SourceOutOfBounds`), two recorders linked to the same clip, a clip with
a speed change.


## The engine's audio processing

> FC5, FILMCRAFT-2026-10. Ported from FilmCraft (`crates/audio-dsp`, MIT or
> Apache-2.0, commit `09aaf9b`).

The engine has its own audio processing library (`src/AudioDsp*.h`), portable
and dependency-free: a six-band parametric equalizer (`parametric_eq`: bell,
low and high shelves, high-pass, low-pass, RBJ sections from the *Audio EQ
Cookbook*), a 12 or 24 dB per octave Butterworth high-pass (`high_pass`), a
compressor (`compressor`: threshold, ratio, knee, attack, release, makeup
gain), a de-esser (`de_esser`: a dynamic high shelf), a look-ahead true-peak
limiter (`limiter`), a spectral denoiser (`denoise`) and a comb of mains notch
filters at 50 or 60 Hz and its harmonics (`de_hum`). **None of it is wired
in**: neither export nor playback uses it. The single mixer that playback and
export would share (MOTION-MARQUE-2026-10) is still to be decided; this
library is what it will be built on.

What each effect guarantees, and what `audio_dsp_tests` verifies:

- it works in place on planar float blocks, of any length, and its output does
  not depend on how the signal is split into blocks, to the bit (blocks of 1,
  64, 1,000 samples, of varying size, empty ones included, a setting changed
  midway);
- it allocates nothing while processing, receiving a setting or resetting;
- it declares its delay (`Latency()`, in samples), and an impulse comes out
  where it says: 271 samples for the limiter at 48 kHz (5 ms of look-ahead and
  the two half-lengths of the true-peak filter), 2,048 for the denoiser (one
  frame), 0 for the others;
- at its neutral setting (all bands at 0 dB, ratio 1, zero reduction, ceiling
  at 0 dBTP on a sound that does not reach it, zero amount, high-pass off), it
  returns the sound as is, delayed by its delay, to the bit; the denoiser at
  0 dB to within 1e-6, the transform and its inverse;
- each setting has a stable identifier, a range, a unit and a default value,
  and reaches a new value through a 20 ms ramp, sample by sample;
- a recursive state below 1e-30 is reset to zero: a long silence after a
  sound leaves no denormal number.

Measured under Linux: the response of each section, measured on a sine,
follows the formula to within 5e-7 dB, and FFmpeg's `equalizer`, `highpass`,
`lowpass`, `lowshelf`, `highshelf` and `bandreject` filters (the same sections,
in double precision; Ubuntu's FFmpeg 6.1.1) give, over one second of noise, the
same floating-point samples for six of them and at most 7e-12 of difference for
the notch; the test tolerance is 1e-6 (`audio_dsp_ffmpeg_tests`, skipped, and
reported as skipped, without FFmpeg); the limiter, pushed by 12 dB, leaves no
true peak above its ceiling according to the meter of `Loudness.h`, whose
oversampling filter it shares (and `NormalizeLoudness` keeps its outputs
bit-identical); the denoiser lowers noise alone by 15.7 dB and raises the
signal-to-noise ratio of a sine from 25.2 to 39.5 dB; the de-hum lowers 50 Hz
by 60 dB and leaves 1 kHz within 0.001 dB. Nothing has been listened to.

## Proposing ducking under a voice

`propose_ducking` (MCP; CLI `--propose-ducking`, FC6, FILMCRAFT-2026-10) reads
the sound of a voice source clip (`media_id`, on the channel `channel`: `mix`,
`left` or `right`) and says where the voice speaks, and which gain curve music
placed under it should follow. **It modifies nothing**: a clip's animated gain
does not exist yet (REEL-2026-09, R2); the curve is a proposal, for the agent
or for whoever mixes. Ported from FilmCraft
(`crates/audio-dsp/src/ducking.rs`).

The sound is decoded at 48 kHz and read in 10 ms frames (their RMS level). The
noise floor is a low percentile of the levels (`floor_percentile`, 10 by
default), frames of digital silence (below −100 dBFS) excluded: the threshold
sits above the room noise, not at an absolute level, like the troughs of
`analyze_timeline_audio`. Speech starts when the level passes `threshold_db`
above the floor (10 dB) and stops when it falls `hysteresis_db` below that
threshold (4 dB): the dip of a word does not cut the range. Gaps shorter than
`bridge_ms` (300 ms) are closed, then ranges shorter than `minimum_speech_ms`
(200 ms) are discarded: a cough, a door.

The curve (`gain_curve`) is at 0 dB outside the voice, goes down by `depth_db`
(12 dB) starting `attack_ms` (150 ms) before each range until its start, holds
until its end, and comes back up over `release_ms` (400 ms). Between two
points, the curve is a straight line in dB, and each point is worth 0 or
−`depth_db`. Two ranges whose ramps would meet stay lowered between them. A
ramp that would start before the sound starts with it; one that would end
after it ends with it: the ramp is then shorter.

Each time is exact: one sample at 48 kHz, as `{value, rate}`, in the source
clip's time. A duration that does not fall on a whole sample is refused
(`InvalidInput`), never rounded; at 48 kHz, every whole millisecond falls on
one. The response gives the levels read (`levels`: floor, voice at the 95th
percentile, input and output thresholds, in dBFS to the hundredth), the
settings applied, the ranges (`speech`), the curve and `applied: false`. A
silent source clip answers `silent: true`, with no range. Like
`measure_audio_offset`, the tool decodes all of the sound: it runs in the
background.

Verified on synthetic voices (`audio_dsp_ducking_tests`: ranges and curve to
the sample, gap closed, cough discarded, dip held by the hysteresis, ramps
merged and clipped at the edges, refusals). Not yet on a real voice, nor
listened to under music.

## Rendering a stem on the cuts

`render_sound` (MCP; CLI `--call-tool <projet> render_sound`, SN7,
SON-2026-10) renders a ditty (the format of
[Dittytoy](https://dittytoy.net), with `edit`, `at()` and `sleepUntil()`: see
[`tools/cutmachine-sound`](../../tools/cutmachine-sound/README.md)) on the cuts
of the timeline as it is in memory, without a prior save. With `partition`,
`ditty` is a set of instruments and the score says what plays and when. The
stem is a 24-bit WAV whose BWF timecode is the start of the timeline
(`01:00:00:00`, or `start_timecode`); without `out`, it goes to
`Sound/<nom>-seed<n>.wav` in the package, and `stems: true` adds one WAV per
loop there.

The rendering is the JavaScript of `tools/cutmachine-sound`, played by
`cutmachine-sound-host` in JavaScriptCore: a separate process, placed next to
the engine, the sole holder of the `allow-jit` entitlement, stopped after
`timeout` seconds (`SoundTimeout`). The response is the report of `cm-sound
render` (`out`, `sha256`, peaks, `loops` and their `triggers` with their
`notes` if requested, `edit`), and the file is the one the Node CLI writes, to
the byte (verified by `sound_project_tests`). A ditty that throws an error, a
score that names a missing cut are refused with the words of the rendering
(`SoundRenderFailed`). Without the host, under Linux, `SoundHostMissing`.
Next: `ingest_media` of the stem, then `place_sound_stem`.

A score can be written from a [Strudel](https://strudel.cc) pattern:
`cm-sound strudel` (Node CLI only, SN9) turns it into notes, at the exact time
of each cycle and, with the edit, up to its end; `instruments/strudel.js`
answers the names of its sounds.

## Placing a rendered stem at its timecode

`place_sound_stem` (MCP; CLI `--call-tool <projet> place_sound_stem`, SN4,
SON-2026-10) places on the timeline a stem that
`tools/cutmachine-sound` rendered from this project (`render --project`),
where its BWF timecode says, in a single undo step. The stem is first ingested
(`ingest_media`); the tool takes its `media_id`, never a time.

The rendering writes in the `bext` chunk the sample of the timeline's first
frame, at the start timecode that the EDL export uses (`01:00:00:00`,
`start_timecode` otherwise), by a single rule: frame `f` falls on sample
`round(f × den × fréquence / num)`, a half rounded up. The tool inverts this
rule: the frame whose sample is the stem's timecode, or a named refusal when
none has it (a stem rendered for another frame rate, another start or another
timeline is never snapped to the neighbouring frame). A stem that would start
before the first frame is refused, as is a file whose length is no longer that
of the media pool, a media that has a picture, or a WAV without `bext`.

The clip covers the whole frames that the stem overlaps, from the first: the
end of the stem within its last frame (a reverb tail, a frequency that does not
divide the frame rate) is not laid, and `placed.not_laid` says how much.
Without `track_id`, an audio track is added above all the others; with it, the
clip is pasted there without ever overlapping what it contains (`Overlap`). The
response names the clip, the track, the first frame and their count
(`placed`). Nothing new in the document: a track addition and a paste, in one
batch (`SoundStem.h`).

Verified by `sound_stem_tests` (BWF header read before or after the data,
exact inversion at 25, 23.976 and 29.97 drop-frame, refusals, undo and redo to
the byte) and `mcp_tools_tests`; on a real project at 23.976 fps, a 44.1 kHz
stem placed on its 234 frames, then removed to the byte by `undo`. Nothing has
been listened to, nor verified in Resolve.
