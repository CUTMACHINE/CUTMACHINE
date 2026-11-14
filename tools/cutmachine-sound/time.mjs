// SN2 -- docs/roadmap/SON-2026-10.md. The one place an edit's time becomes samples.
//
// An edit counts in frames, exactly. Its seconds (`cut.time`, `shot.start`) are rounded to the
// microsecond for reading, and a second rounding into samples can then miss by one: at 23.976 fps
// and 44.1 kHz, frame 40 falls on sample 73573.5 exactly, which rounds up to 73574, while
// 1.668333 s gives 73573. So a frame goes to its sample here, in integers, rounding a half up,
// as Math.round does for the seconds a ditty gives itself.

const big = (x, what) => {
    if (!Number.isSafeInteger(x)) throw new RangeError(`${what} must be a whole number, got ${x}`);
    return BigInt(x);
};

// a / b rounded down, for any signs.
const floorDiv = (a, b) => {
    const q = a / b;
    return a % b !== 0n && (a < 0n) !== (b < 0n) ? q - 1n : q;
};

// ─────────────────────────────────────────── exact rationals
//
// SN5 -- a partition's instants are a frame of the edit, plus beats on a tempo and seconds it
// chooses (a lead, an offset). They are summed here as exact fractions over BigInt, and rounded
// into samples once (toSample), so that a sum never carries the rounding of its terms.

const gcd = (a, b) => {
    a = a < 0n ? -a : a;
    b = b < 0n ? -b : b;
    while (b) [a, b] = [b, a % b];
    return a;
};

/** n / d, reduced, d > 0. */
export function rational(n, d = 1n) {
    if (d === 0n) throw new RangeError('a fraction cannot have a zero denominator');
    if (d < 0n) { n = -n; d = -d; }
    const g = gcd(n, d) || 1n;
    return { n: n / g, d: d / g };
}
export const add = (a, b) => rational(a.n * b.d + b.n * a.d, a.d * b.d);
export const sub = (a, b) => rational(a.n * b.d - b.n * a.d, a.d * b.d);
export const mul = (a, b) => rational(a.n * b.n, a.d * b.d);
export function div(a, b) {
    if (b.n === 0n) throw new RangeError('division by zero');
    return rational(a.n * b.d, a.d * b.n);
}
export const compare = (a, b) => { const x = a.n * b.d, y = b.n * a.d; return x < y ? -1 : x > y ? 1 : 0; };
/** For synthesis and reading only: never for a placement. */
export const toNumber = (r) => Number(r.n) / Number(r.d);

/**
 * The exact value of what a partition writes: a JSON number (its binary value, exactly: 0.375 is
 * 3/8, 0.1 is the double nearest to it), or a text, "p/q" (a triplet, "1/3") or a decimal ("0.1").
 */
export function exact(x, what = 'value') {
    if (typeof x === 'number') {
        if (!Number.isFinite(x)) throw new RangeError(`${what} must be a finite number, got ${x}`);
        let v = x, d = 1n;
        while (!Number.isInteger(v)) { v *= 2; d *= 2n; }              // exact: a double times 2
        return rational(BigInt(v), d);
    }
    if (typeof x === 'string') {
        let m = x.trim().match(/^(-?\d+)\s*\/\s*(\d+)$/);
        if (m && BigInt(m[2]) !== 0n) return rational(BigInt(m[1]), BigInt(m[2]));
        m = x.trim().match(/^(-?)(\d+)(?:\.(\d+))?$/);
        if (m) {
            const decimals = m[3] ?? '';
            const n = BigInt(m[2] + decimals) * (m[1] ? -1n : 1n);
            return rational(n, 10n ** BigInt(decimals.length));
        }
    }
    throw new TypeError(`${what} must be a number, "p/q" or a decimal text, got ${JSON.stringify(x)}`);
}

/** The sample an exact time in seconds falls on, at `sampleRate`: a half rounds up. */
export function toSample(seconds, sampleRate) {
    const n = seconds.n * big(sampleRate, 'the sample rate');
    return Number(floorDiv(2n * n + seconds.d, 2n * seconds.d));
}

/** A frame of the edit as an exact time: frame × den / num seconds, a half frame allowed. */
export function frameTime(frame, fps) {
    const twice = frame * 2;
    if (!Number.isSafeInteger(twice)) throw new RangeError(`frame ${frame} is neither a whole nor a half frame`);
    const num = big(fps.num, 'fps.num');
    if (!(num > 0n)) throw new RangeError('fps.num must be > 0');
    return rational(BigInt(twice) * big(fps.den, 'fps.den'), 2n * num);
}

// ─────────────────────────────────────────── frames to samples

/**
 * The sample a frame of the edit falls on: round(frame × den × sampleRate / num), exactly.
 * `frame` is a whole frame, or a half one -- the middle of a transition of an odd number of frames.
 */
export function frameToSample(frame, fps, sampleRate) {
    return toSample(frameTime(frame, fps), sampleRate);
}

/**
 * How many samples cover `frames` whole frames from the start: frames × den × sampleRate / num,
 * rounded up. A stem's length, unlike an instant, never rounds down: a stem 0.4 sample short of
 * its last frame (176 frames at 23.976 fps and 44.1 kHz) would not hold that frame, and the engine
 * lays only the frames a stem holds whole (SoundStem.h).
 */
export function framesToSamplesCovering(frames, fps, sampleRate) {
    if (!Number.isSafeInteger(frames) || frames < 0) throw new RangeError(`${frames} is not a whole number of frames`);
    const num = BigInt(frames) * big(fps.den, 'fps.den') * big(sampleRate, 'the sample rate');
    const den = big(fps.num, 'fps.num');
    return Number((num + den - 1n) / den);
}

