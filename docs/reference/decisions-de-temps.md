# Time decisions

CUTMACHINE loads a v2 project package, opens each media source by its ULID,
then resolves scrubbing into cache keys `(source_id, source_frame)`.
Clip intervals are half-open: `[timeline_in, timeline_in + duration)`. A gap
triggers no decoding and is rendered black.

- `RationalTime::rescale` refuses inexact conversions so that a cut is never
  silently moved.
- `to_frames` rounds down to the frame that contains the position and
  accepts a rational rate, for example `30000/1001`.
- `TimelineViewport` is the single time/pixel boundary. `TimeToX` keeps the
  intermediate computation in `long double`; `XToTime(x, rate)` rounds to the
  nearest tick of the requested timebase, with half-ticks rounded away from
  zero. In the NTSC timebase `30000`, the frame boundaries of `30000/1001`
  are the multiples of `1001` ticks, so their round trip is exact. This
  symmetric choice also makes views with a negative `view_start`
  deterministic.
- `sources[].rate` is interpreted as a rational rate, while the `rate` of a
  `RationalTime` is an integer timebase. So a frame at `30000/1001` lasts
  `1001` ticks in a `30000` timebase; this convention is necessary because
  `RationalTime::rate` cannot itself hold a quotient.
- A source frame number names a picture through the source's grid
  (`SourceGrid.h`): frame `k` is the picture on screen `k / rate` after the
  source's first picture, as FFmpeg's `fps` filter resamples the stream
  anchored on that picture — the latest picture whose time, in grid steps
  and rounded to the nearest step (halves away from zero), is `k` or less.
  On a constant-rate file it is the file's `k`-th picture; on a phone's
  variable-rate file it is the picture nearest `k / rate`, so the picture
  stays with the sound, which is placed by time. The export and
  `verify_export` read a source on this grid through the same function; the
  monitor decodes frame `k` as the first picture whose rounded step reaches
  `k`, the rounding it names pictures with during playback.
- The rate a source is counted at (`sources[].rate`) is chosen once, at
  ingest, by `SourceCountingRate`: the stream's average rate, except for a
  variable-rate stream whose average is no standard cadence, whose nominal
  rate (`r_frame_rate`) is one, and which stays within 2 % of it — then the
  nominal rate, the grid the camera recorded on. A genuine `24000/1001`
  file is never counted at `24`. A media keeps the rate read at its import.
- The playhead has two grids, toggled by `Alt+Shift+M`. In Image mode, its
  position is rounded to the nearest frame according to the rational
  reference rate (that of the sequence; failing that, the first source),
  including the multiples of `1001` at `30000/1001`. In Échantillon (Sample)
  mode, it is rounded to the nearest 48 kHz sample. Half-steps are rounded
  away from zero by 128-bit integer arithmetic; no time passes through
  `double` for this quantization.
- Video tracks are ordered by their `index` field and composited from bottom
  to top. A gap on an upper track reveals the lower tracks.
- Video media carrying an alpha channel are composited in Metal. Each video
  clip also has an exact `num/den` opacity, editable in the Inspector or by
  the `SetClipOpacity` operation; fades multiply this value and the opaque
  export takes it into account.
