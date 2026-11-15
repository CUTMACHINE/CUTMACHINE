# Shot quality control

> Measurements on real takes (values, corpus, threshold sweeps) are not
> verified by reading: only the thresholds that come out of them are.

A blurry or shaky shot is not judged, it is measured. `--shot-quality` analyses
a media at four samples per second and writes a cache in
`.cutmachine/shotquality/<media-id>.json`; `--shot-quality-report` (MCP:
`list_shot_quality`) rates each clip of the active timeline.

Two measurements, no inference:

- **Sharpness** — variance of the Laplacian on the luminance plane, rated
  against the median of the media itself. The relative scale is deliberate: the
  variance of the Laplacian has no absolute meaning from one content to
  another, whereas within a single source clip, same camera and same optics,
  the comparison holds. The price of this choice is stated rather than hidden —
  a media that is blurry from end to end rates all its shots "Sharp", hence the
  publication of the absolute medians next to the rating.
- **Shake** — mean absolute luminance difference between two samples, rated in
  absolute terms: the quantity means the same thing on any source clip.

The grade rests on the **worst decile**, not on the median: a shot that loses
focus midway stays healthy in the median and stays unusable.

Measured on a real take (hand-held interview, 6.72 s, four samples per second,
1.7 s of analysis): the body of the shot holds between 4,055 and 4,481 of
sharpness, and the last five samples collapse to 3,499, 3,691, 2,616, 1,461
then 336 while the shake goes from 5,000–36,000 to 67,624. Checked on the
image: the camera leaves the subject at the end of the take, the frame drifts
onto a wall and the last image is nothing but mush. This is exactly what the
measurement must catch. A clip placed on this end is rated `Blurry` / `Shaky`,
the one placed on the body `Sharp` / `Steady`.

The shake thresholds are calibrated on this single take. It is an anchor point,
not a calibration: each rating is published with the number that produced it,
precisely so that they move on evidence.

A media that was never analysed appears under `unanalyzed`, never among the
clean shots. The absence of a measurement is not a green light:
`analyze_shot_quality` (MCP) starts the analysis from the agent, without going
through the command line.

A cache written in a format that this binary no longer reads (version 1 or 2,
or a more recent version) is not a media that was never analysed: its shots
stay under `unanalyzed` with a reason that says so, and `unreadable_caches`
names each source, the version found, the readable versions and the call that
rebuilds the cache (`analyze_shot_quality`, `--shot-quality` in the CLI); the
other sources are rated as usual. Nothing is reanalysed unless asked.
`list_stable_windows`, which covers a single source, refuses
(`"error":"UnreadableCache"`, same `result.unreadable_caches`).

The rating takes overlap into account. A soft shot entirely hidden by an
illustration placed above it is not a defect to fix, since no one sees it —
each entry therefore carries its rating (`clean`, what is measured), its
duration still visible (`visible`, what the document says) and
`needs_attention`, which is the conjunction of the two. Without this, the view
asked for cutting away invisible shots.

## Splitting a source clip into shots

Rating what is already edited is one thing; knowing what the source clips
contain is another. A file that comes out of the camera as a single take can
hold six takes, and as long as they are not named there is nothing to select,
rate or describe other than the whole file.

`--shot-quality-report` therefore publishes, next to the list of clips, a
`sources` table: for each analysed media, the shots detected inside it, with
their start, their end and the sharpest sample — the image to extract when a
single one must represent the shot. The times are in the source's time domain,
hence usable as they are as `source_in`.

One measurement remains, without a model: a cut is a discontinuity of the
image. A third quantity per sample is added to the previous two, the **luma
histogram distance** — the fraction of the pixels that changed luminance bin.
The cache goes to version 3; the old ones are refused, to be reanalysed, rather
than completed with a value that was never measured.

The initial idea was that a pan moves the image without changing its
distribution, unlike a cut. **Measured, this idea is wrong.** On the real take
above, the instant when the camera leaves the subject reaches 70,433 of shake
and 292,944 of histogram distance; a real cut edited in the same material
measures 48,489 and 30,900. The camera movement rates *higher than the cut on
both quantities*. No pair of absolute thresholds separates them.

What separates them is not the amplitude of the change but its shape. A camera
has inertia: a movement spreads over several samples. A cut is instantaneous:
it falls on a single one. Relative to the median of their neighbourhood, the
same events sort themselves out — the camera movements rise to 153%, 194%,
201%, 202%, 210%, 232% and 299%, the two real cuts to 408% and 3,956%. The
threshold is at 350%, equidistant from both, and 162 of the combinations swept
around this point give the same result on the four fixtures: the rule sits on
a plateau, not on an edge.

The limits are known and stated: a dissolve is not found, since it has no
isolated sample to latch onto; a cut between two really similar shots may fall
under the absolute floors; and the calibration rests on four fixtures of which
only one is real material. Widening this corpus is what would stop the numbers
above being provisional.

## Camera movement and stable windows

Shake does not answer the question "is the camera moving": it rates a speaker
who gesticulates on a tripod like a shoulder-mounted shot, and does not see a
zoom. The analysis therefore also measures the **global motion** between two
samples, by phase correlation on a grid of nine blocks: horizontal and vertical
translation, and zoom deduced from the spread between the blocks'
displacements. A sample where fewer than four blocks are reliable (uniform
image or one that changes too fast) is declared not measured, never still. The
cache goes to version 4; a version 3 cache stays readable, without camera
movement.

`list_shot_quality` adds to each shot a `camera` block: median speed, 90th
percentile speed and zoom peak. `list_stable_windows` (MCP) lists the portions
of a source clip where the camera holds: speed under `max_speed`, zoom under
`max_zoom_per_mille` per second and sharpness above `min_sharpness_percent` of
the source clip's median, for at least `min_duration_ms`. Speeds are in pixels
per second relative to an image 1,080 pixels wide, whatever the definition of
the source clip. Each window is returned in source clip frames with an exact
`source_in` and duration, ready for a cutaway.

The response also carries a `rush` block, whether there is a window or not:
median speed, share of the measured intervals beyond each limit
(`too_fast_percent`, `too_much_zoom_percent`, `too_soft_percent`), longest
range within all the limits (`longest_still`, on the analysis grid, even
shorter than `min_duration_ms`), measured and unmeasured intervals. An empty
list of windows does not mean that the source clip is unusable: the best shots
of a photo shoot, hand-held tracking shots at 110–260 px/s in median, can fall
outside every window. The measurement says that the camera moves; it does not
say whether that is intended. On that shoot, a tracking shot that keeps its
subject and the end of a take that loses it measure the same, and the tracking
shot even looks the less steady of the two, at four samples per second as at
the frame rate: no measurement decides it, the image settles it.

The defaults (80 px/s, 150 ‰/s, 72%, 1.2 s) are calibrated on the 23
shoulder-mounted source clips of a real interview, where the camera moved at 73
px/s in median: they open a window in 11 source clips. They follow the edit
judged best on these source clips; the first defaults, 60 px/s and 1.6 s,
opened only 6 and removed natural drifts. The zoom estimate confuses parallax
and a moving subject with a zoom: its median there was 54 ‰/s, and a limit
under about 100 rules out any shoulder-mounted shot. 150 still stops a
deliberate zoom.

## Isolated frames

Four samples per second do not see a single frame. On a film shot during a
photo shoot, a studio flash raised the mean luminance of one frame from 104 to
182 before it went back down: at 50 fps, the analysis above misses it eleven
times out of twelve, and only the check render found it.

`--isolated-frames` (MCP: `list_isolated_frames`, `analyze_isolated_frames`)
therefore goes **frame by frame**, read-only, but only over the **source ranges
that the timeline reads**, at each clip's speed and transition handles
included: a two-second clip at 2/1 is measured from source frames 0 to 98, plus
the margin (PRD38). For a 74 s film drawn from 104 source clips, that is a
handful of seconds per source clip instead of their whole length. Two
quantities per frame, as integers: the **mean luminance** in thousandths of a
level (104.0 is written 104000) and the **luma histogram** in per-mille of
pixels per bin. The cache (`.cutmachine/isolatedframes/<media-id>.json`)
contains no floating-point number and two passes write the same bytes.

A deviation of **one or two frames** that **returns to the level of its
neighbours** is flagged. The rule separates the three false positives by shape,
not by amplitude: a cut moves the level and leaves it there, a lighting ramp
moves it away from a whole neighbourhood, a pan never settles. The two
triggers — level deviation of at least 18 levels, or 15% of the pixels changing
bin — are also related to the neighbourhood's own motion, as the shot
boundaries are above.

For the **dérush** (the review of source clips), before anything is edited,
`--media <id>` (repeated; MCP: `media_ids`) measures these source clips **in
full**, whether or not a timeline reads them. This is where the flashes of a
photo shoot are spotted, to keep or to discard them; there is no need to edit
each source clip into a dérush timeline to have ranges to measure.

The tool **locates, it does not judge**. The two more rhythmic versions of the
same film make a figure of these flashes: eight clips start on the flash frame,
set on a beat. Nothing is therefore named "error"; each frame is returned with
its deviation, the level it returns to and the clips that play it. On a source
clip of which a clip is not at 1/1, these clips are found by the speed rule of
the monitor and of the export: a frame that a sped-up clip skips is played by
no clip, and a 1/1 clip of a double-rate source clip plays only one in two. A
cache written before PRD38 appears under `unscanned`.

The same measurement runs on the delivered file, in `verify_export`, on the
frames that its comparison counts `unverifiable` — precisely where a flash
hides, at half opacity in a fade handle, where it is still found.

These thresholds are calibrated on the only case measured (the flash above,
whole then halved) and on synthetic series. Unlike those of the quality
control, no corpus of source clips has swept them: this is what would stop them
being provisional.
