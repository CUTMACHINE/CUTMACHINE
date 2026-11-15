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
