# Reference images in tests

FC9, FILMCRAFT-2026-10. A render made on
the CPU is compared with a PNG image that someone has looked at, stored under
`tests/fixtures/golden/`, rather than only with a few pixels picked by the
test. The module is `tests/golden_image.h` (taken from FilmCraft,
`crates/testkit/src/golden.rs`, MIT or Apache-2.0); it tests itself in
`cutmachine_golden_image_tests`, and is used today for the motion effects
(blur, drop shadow, glow, displacement; grain, goo and slices from MA7) in
`cutmachine_motion_parity_tests`.

## Using it

```cpp
#include "golden_image.h"

GoldenCheck check;
check.name = "motion_effects_blur";            // tests/fixtures/golden/<name>.png
check.reference_dir = GOLDEN_REFERENCE_DIR;
check.output_dir = GOLDEN_OUTPUT_DIR;
check.depth = GoldenDepth::Sixteen;            // or Eight
check.tolerance = {90.0, 1.0, 0.0};            // min PSNR, max difference, p99
const GoldenResult result = CheckGoldenImage(
    GoldenImageFromFloat(width, height, 4, rgba), check);
Check(result.passed, result.message);
```

The suite that uses it compiles `tests/golden_image.cc` with its sources and
receives the two directories through `target_compile_definitions` (see
`cutmachine_motion_parity_tests` in `CMakeLists.txt`). An image is floats in
[0, 1], RGB or RGBA, interleaved, top row first; bytes or 16-bit words convert
to it (`GoldenImageFromBytes`, `GoldenImageFromWords`). The PNG goes through
libavcodec, which the model already links: no extra dependency.

## The three measures

The render is first rounded to the depth of the reference (8 or 16 bits), as
the reference was when it was written: an unchanged render compares at zero on
the machine that wrote it. Differences are in levels of that depth (0 to 255,
or 0 to 65,535).

- **PSNR**: over all samples, in dB, against full scale; infinite for two
  identical images.
- **Max difference**: the largest difference of any one sample.
- **p99**: the 99th percentile, over pixels, of each pixel's largest
  difference. What 99% of the image meets: a few pixels on a rounding edge do
  not hide an offset of the whole image, nor the reverse.

Each test declares its tolerance; it is exact by default.

## A failure

The message gives the three measures, the tolerance, and two paths, in the
test's output directory (`build…/golden-output/`, or
`CUTMACHINE_GOLDEN_OUTPUT` if it is set):

- `<name>.actual.png`, the render as it was compared;
- `<name>.diff.png`, the difference of each channel multiplied by eight, with
  the alpha difference carried onto the three colour channels, opaque.

A missing reference makes the test fail; it still writes the render so that
it can be looked at.

## Rewriting the references

```sh
CUTMACHINE_BLESS=1 ./build-headless/cutmachine_motion_parity_tests
```

`CUTMACHINE_BLESS` set to `1`, `true`, `yes` or `on` rewrites each reference
instead of comparing it. A reference over 64 KiB is refused: make it smaller.
Look at the rewritten image, and at its difference from the old one in review,
before committing it: a reference rewritten without being looked at proves
nothing.

## Why these tolerances

As long as a render is
not exact from one machine to another, it is compared against a declared and
measured tolerance.

The motion effects are loops in `float` and `double`, plus `std::exp` (the
Gaussian kernel) and `std::floor` (the noise grid). From one machine to
another the last bits change: the compiler fusing a multiplication and an
addition into an FMA where the target has one (always on arm64, on x86-64
only if compiled for it), and the libm `exp`. Such a difference moves a
sample by far less than one level; it only shows where the float lands on a
rounding edge: one level, on a few pixels. The tolerance of these three tests
therefore lets one level through on any sample, keeps p99 at zero and a PSNR
of at least 60 dB (8 bits) or 90 dB (16 bits). A real change (a kernel one
pixel wider, a shadow offset by one pixel, another seed) moves whole edges by
many levels and fails all three.

Measured under Linux x86-64 (clang 18): `MotionEffects.cc` and the scenes
recompiled differently from the default build (`-O3`, which wrote the
references), raw float render compared with that of the default build, then
the rounded render compared with the references.

| Build | Blur σ 4 | Shadow and glow | Displacement | Against the references |
|---|---|---|---|---|
| `-O0` | identical | identical | identical | 0 everywhere |
| `-march=x86-64-v3 -ffp-contract=fast` (FMA) | identical | 790 samples, at most 6.0 × 10⁻⁸ | identical | 0 everywhere |
| `-march=x86-64-v3 -ffast-math` | identical | 1,232 samples, at most 1.2 × 10⁻⁷ | identical | 0 everywhere |

The worst raw difference is 3 × 10⁻⁵ level in 8 bits and 8 × 10⁻³ level in
16 bits: no rounding moved. The one-level tolerance therefore has a margin
that the measurement does not use up; it covers the case, possible but not
observed, of a sample exactly on a rounding edge.

In the other direction, what the tolerance catches, measured the same way: a
shadow offset by one pixel (PSNR 31.9 dB, max difference 58, p99 49), a glow
of strength 2.05 instead of 2 (47.3 dB, max 6, p99 4), another displacement
seed (14.2 dB, max 255), a blur of σ 5 instead of 4 (39.7 dB, max 2,570 in 16
bits). A blur of σ 4.1 or 4.5 changes nothing: SVG's three-box blur rounds σ
to a whole box width, the same (8) from σ 3.99 to 4.52.

That is not a Mac measurement: neither Apple's `exp` nor arm64 enter into it.
The first run of the suite on a Mac gives it; if it exceeds the tolerance, it
is the tolerance that gets reviewed, with figures, not the reference that gets
rewritten on the Mac.

On a Mac (arm64, Apple clang 17), the three references written under Linux
pass at zero. MA7
(MOTION-AFTER-2026-09) adds three more,
written on that Mac: `motion_effects_grain` (grain of 0.3, 2-pixel cells),
`motion_effects_goo` (goo of σ 3) and `motion_effects_slice` (6-pixel slices
on bands of 8), same scene, same tolerance. They have not been compared under
Linux. The grain only does exact computations (whole cells, hashing); the goo
passes the blur through a steep ramp around half coverage, which multiplies a
difference by less than 20; a slice shifts a line by a whole number of
rounded pixels: a one-ulp difference on a value sitting at the half-way point between
two integers would shift a whole line. If the first run under Linux finds one, that is
where to look.

Rewriting the references (`CUTMACHINE_BLESS=1`) on a Mac also rewrites the
three written under Linux, with different bytes for the same pixels
(observed): commit only the ones you meant to change.
