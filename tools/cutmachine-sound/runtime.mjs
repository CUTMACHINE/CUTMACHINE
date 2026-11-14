// cutmachine-sound — offline, deterministic renderer for Dittytoy-style ditties.
//
// Runs the Dittytoy API (loop, sleep, synth.def, filter.def, input, ...) and renders to buffers,
// with three additions for editing:
//   - `edit`: the cut list of a timeline (shots and cuts in seconds and frames), read-only
//   - `at(when, fn)`: schedule plays at an absolute time, in any order
//   - `sleepUntil(when)`: advance a loop to an absolute time
// `when` is seconds, or a cut, a shot or { frame, offset? } of the edit: placed from its frame,
// converted to samples once (time.mjs), so that a cut lands on its exact sample.
//
// SN5 -- and a fourth: `instrument(name, synth, { play, chain, note })` names a synth for a
// partition (partition.mjs). Given one, the renderer plays it with the ditty's instruments: one
// loop per track, each event on its exact sample, whatever the instruments are.
// Math.random() is replaced by seeded streams (one per loop, voice and filter), so the same
// ditty + edit + seed + inputs always renders the same samples.

import vm from 'node:vm';
import { frameToSample, framesToSamplesCovering } from './time.mjs';
import { readPartition, schedulePartition } from './partition.mjs';

const TAU = Math.PI * 2;

// ─────────────────────────────────────────── seeded randomness

const mix32 = (h) => {
    h = Math.imul(h ^ (h >>> 16), 0x85ebca6b);
    h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
    return (h ^ (h >>> 16)) >>> 0;
};
export const seedOf = (...parts) => parts.reduce((h, p) => mix32(h ^ mix32(((p | 0) + 0x9e3779b9) | 0)), 0x2545f491);
export function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// ─────────────────────────────────────────── helpers exposed to ditties

const midi_to_hz = (m) => 440 * Math.pow(2, (m - 69) / 12);
const hz_to_midi = (f) => 69 + 12 * Math.log2(f / 440);
const clamp = (t, lo, hi) => (t < lo ? lo : t > hi ? hi : t);
const clamp01 = (t) => clamp(t, 0, 1);
const lerp = (a, b, t) => a + (b - a) * t;

function noteConstants() {
    const names = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
    const out = {};
    for (let oct = 0; oct <= 9; oct++)
        for (const [n, s] of Object.entries(names))
            for (const [suffix, d] of [['', 0], ['s', 1], ['b', -1]]) {
                const v = 12 * (oct + 1) + s + d;
                out[n + suffix + oct] = v;
                out[n.toUpperCase() + suffix + oct] = v;
            }
    return out;
}

// Runs inside the sandbox so that arrays and objects belong to the ditty's realm.
const PRELUDE = String.raw`
Math.random = __random;
const __def = (proto, name, fn) => Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true });
__def(Array.prototype, 'choose', function () { return this[Math.floor(Math.random() * this.length)]; });
__def(Array.prototype, 'ring', function (i) { const n = this.length; return this[((Math.floor(i) % n) + n) % n]; });
__def(Array.prototype, 'mirror', function () { return this.concat(this.slice().reverse()); });
for (const k of ['choose', 'ring', 'mirror'])
    __def(String.prototype, k, function (...a) { return Array.prototype[k].apply(this.split(''), a); });
globalThis.chords = {
    major: [0, 4, 7], minor: [0, 3, 7], dim: [0, 3, 6], aug: [0, 4, 8], sus2: [0, 2, 7], sus4: [0, 5, 7],
    '6': [0, 4, 7, 9], m6: [0, 3, 7, 9], major7: [0, 4, 7, 11], maj7: [0, 4, 7, 11], dom7: [0, 4, 7, 10], '7': [0, 4, 7, 10],
    minor7: [0, 3, 7, 10], m7: [0, 3, 7, 10], dim7: [0, 3, 6, 9], m7b5: [0, 3, 6, 10], add9: [0, 4, 7, 14],
    m9: [0, 3, 7, 10, 14], maj9: [0, 4, 7, 11, 14], dom9: [0, 4, 7, 10, 14], '9sus4': [0, 5, 7, 10, 14], m11: [0, 3, 7, 10, 14, 17],
};
globalThis.scales = {
    major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10], ionian: [0, 2, 4, 5, 7, 9, 11], aeolian: [0, 2, 3, 5, 7, 8, 10],
    dorian: [0, 2, 3, 5, 7, 9, 10], phrygian: [0, 1, 3, 5, 7, 8, 10], lydian: [0, 2, 4, 6, 7, 9, 11],
    mixolydian: [0, 2, 4, 5, 7, 9, 10], locrian: [0, 1, 3, 5, 6, 8, 10], harmonic_minor: [0, 2, 3, 5, 7, 8, 11],
    melodic_minor: [0, 2, 3, 5, 7, 9, 11], major_pentatonic: [0, 2, 4, 7, 9], minor_pentatonic: [0, 3, 5, 7, 10],
    blues_minor: [0, 3, 5, 6, 7, 10], whole_tone: [0, 2, 4, 6, 8, 10], chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
};
globalThis.chord = (base, notes = [0]) => notes.map((n) => base + n);
globalThis.scale = (base, notes, octaves = 1) => {
    const out = [];
    for (let o = 0; o < octaves; o++) for (const n of notes) out.push(base + n + 12 * o);
    return out;
};
const __freeze = (o) => { if (o && typeof o === 'object') { Object.values(o).forEach(__freeze); Object.freeze(o); } return o; };
globalThis.edit = __editJson
    ? __freeze(JSON.parse(__editJson))
    : new Proxy({}, { get(_, k) {
        if (typeof k === 'symbol') return undefined;
        throw new Error('this ditty reads edit.' + String(k) + ' but no edit was given (use --edl or --edit)');
    } });
delete globalThis.__editJson;
`;

const isClass = (f) => typeof f === 'function' && /^\s*class\b/.test(Function.prototype.toString.call(f));

function resolve(opts, tick) {
    const r = {};
    for (const k in opts) {
        const v = opts[k];
        r[k] = typeof v === 'function' ? v(tick, r) : v;
    }
    return r;
}
const hasFunctions = (opts) => Object.values(opts).some((v) => typeof v === 'function');

// ─────────────────────────────────────────── envelopes

const seg = (x, c) => (Math.abs(c) < 1e-6 ? x : (1 - Math.exp(c * x)) / (1 - Math.exp(c)));

function makeEnv(kind, o, bpm) {
    const tps = 60 / bpm;
    if (kind === 'one') {
        const end = (o.duration ?? 1e5) * tps;
        return { value: 1, done: false, update(ts) { if (ts >= end) { this.done = true; this.value = 0; } } };
    }
    // adsr: attack, decay, release in seconds; sustain is a level; duration in ticks.
    // Attack and decay always run to the end, even when duration is shorter.
    const a = Math.max(0, o.attack ?? 0), d = Math.max(0, o.decay ?? 0), r = Math.max(0, o.release ?? 0.5);
    const s = o.sustain ?? 1;
    const c = o.curve ?? -2;
    const [ca, cd, cr] = Array.isArray(c) ? [c[0] ?? -2, c[1] ?? c[0] ?? -2, c[2] ?? c[1] ?? c[0] ?? -2] : [c, c, c];
    const gate = Math.max((o.duration ?? 0) * tps, a + d);
    return {
        value: 0,
        done: false,
        update(ts) {
            let v;
            if (ts < a) v = seg(ts / a, ca);
            else if (ts < a + d) v = 1 + (s - 1) * seg((ts - a) / d, cd);
            else if (ts < gate) v = s;
            else {
                const rt = ts - gate;
                if (rt >= r) { this.done = true; v = 0; } else v = s * (1 - seg(rt / r, cr));
            }
            this.value = v;
        },
    };
}

// ─────────────────────────────────────────── render

/**
 * Render a ditty.
 * @param {object} o
 * @param {string} o.code          ditty source
 * @param {object} [o.edit]        edit object (see edl.mjs); sets the render length
 * @param {number} [o.duration]    render length in seconds when there is no edit
 * @param {number} [o.sampleRate]  default 48000
 * @param {number} [o.seed]        default 1
 * @param {object} [o.inputs]      values for input.* sliders, overriding the ditty's defaults
 * @param {number} [o.tail]        extra seconds rendered after the end (reverb and echo tails)
 * @param {boolean} [o.stems]      also return each loop's output, before shared filters
 * @param {object} [o.partition]   a partition (partition.mjs) to play with the ditty's instruments
 * @param {boolean} [o.triggers]   also return each loop's trigger samples, and their notes
 */
export function render(o) {
    const SR = o.sampleRate ?? 48000;
    const seed = o.seed ?? 1;
    const overrides = o.inputs ?? {};
    const messages = [];
    const say = (level, text) => {
        if (messages.length < 200 && !messages.some((m) => m.text === text)) messages.push({ level, text });
    };

    let current = null;            // loop being evaluated
    let globalBpm = 120;
    let renderSample = 0;
    let rand = mulberry32(seedOf(seed, 0));

    const ditty = {
        get bpm() { return current ? current.bpm : globalBpm; },
        set bpm(v) {
            if (!(v > 0)) throw new RangeError('ditty.bpm must be > 0');
            if (current) current.bpm = v; else globalBpm = v;
        },
        get sampleRate() { return SR; },
        get dt() { return 1 / SR; },
        get dtick() { return this.bpm / 60 / SR; },
        get tick() { return current ? current.tick : renderSample / SR * globalBpm / 60; },
        get time() { return current ? current.sec : renderSample / SR; },
        get seed() { return seed; },
    };

    // synths
    const synth = {
        def(impl, defaults = {}) {
            if (typeof impl !== 'function') throw new TypeError('synth.def expects a function or a class');
            const def = { impl, isClass: isClass(impl), defaults };
            return {
                play(note, options = {}) {
                    if (!current) throw new Error('play() must be called inside a loop');
                    const ev = {
                        sec: current.sec, exact: current.exact, def, note, loop: current.index, n: current.events.length,
                        opts: Object.assign({ amp: 1, pan: 0 }, defaults, options),
                    };
                    current.events.push(ev);
                    return {
                        options: new Proxy({}, { set() { say('warn', 'voice.options.x = ... is not supported yet and was ignored'); return true; } }),
                    };
                },
            };
        },
    };

    // filters
    const filters = [];
    const filter = {
        def(impl, defaults = {}) {
            if (typeof impl !== 'function') throw new TypeError('filter.def expects a function or a class');
            const def = { impl, isClass: isClass(impl), defaults };
            const make = (options, shared) => {
                const f = { def, opts: Object.assign({}, defaults, options), shared, index: filters.length, used: false };
                filters.push(f);
                return f;
            };
            return { create: (opts = {}) => make(opts, false), createShared: (opts = {}) => make(opts, true) };
        },
    };

    // loops
    const loops = [];
    function loop(fn, options = {}) {
        if (current) throw new Error('loop() must be called at the top level, not inside another loop');
        if (typeof fn !== 'function') throw new TypeError('loop expects a function or a class');
        const L = {
            index: loops.length, fn, isClass: isClass(fn),
            opts: Object.assign({ amp: 1, pan: 0, name: `loop ${loops.length}` }, options),
            chain: [], events: [], tick: 0, sec: 0, exact: undefined, bpm: 120,
        };
        loops.push(L);
        const handle = {
            connect(f) {
                if (!f || !f.def || !filters.includes(f)) throw new TypeError('connect() expects a filter made with .create() or .createShared()');
                const last = L.chain[L.chain.length - 1];
                if (last && last.shared) throw new Error(`${L.opts.name}: nothing can be connected after a shared filter`);
                if (!f.shared) {
                    if (f.used) throw new Error('a filter made with .create() can only be connected once; use .createShared() to share one');
                    f.used = true;
                }
                L.chain.push(f);
                return handle;
            },
        };
        return handle;
    }
    const needLoop = (what) => { if (!current) throw new Error(`${what} must be called inside a loop`); };
    // A loop's position is seconds, and also an exact sample (`exact`) while it stands where an
    // edit's frame put it: a play there starts on that sample, not on its seconds rounded again.
    const advance = (ticks) => {
        current.tick += ticks;
        current.sec += ticks * 60 / current.bpm;
        current.exact = undefined;
    };
    // An instant: seconds (a number), or anything with a `frame` of the edit -- a cut, a shot,
    // { frame: c.centerFrame } -- plus an optional `offset` in seconds, as in { frame, offset: -0.4 }.
    function instant(when, what) {
        if (typeof when === 'number') {
            if (!Number.isFinite(when)) throw new RangeError(`${what}(${when}): expected seconds`);
            return { sec: when, sample: undefined };
        }
        if (when && typeof when === 'object' && typeof when.frame === 'number') {
            if (!o.edit) throw new Error(`${what}() was given a frame of the edit, but no edit was given (use --edl, --edit or --project)`);
            let sample = frameToSample(when.frame, o.edit.fps, SR);
            if (when.offset !== undefined) {
                if (!Number.isFinite(when.offset)) throw new RangeError(`${what}(): offset ${when.offset} is not a number of seconds`);
                sample += Math.round(when.offset * SR);
            }
            return { sec: sample / SR, sample };
        }
        throw new TypeError(`${what}(${when}): expected seconds, or a cut, a shot or { frame } of the edit`);
    }
    function sleep(ticks) {
        needLoop('sleep()');
        if (!Number.isFinite(ticks) || ticks < 0) throw new RangeError(`sleep(${ticks}): expected a number of ticks >= 0`);
        advance(ticks);
    }
    function sleepUntil(when) {
        needLoop('sleepUntil()');
        const t = instant(when, 'sleepUntil');
        if (t.sec > current.sec) advance((t.sec - current.sec) * current.bpm / 60);
        if (t.sec >= current.sec) { current.sec = t.sec; current.exact = t.sample; }
    }
    function at(when, fn) {
        needLoop('at()');
        const t = instant(when, 'at');
        if (typeof fn !== 'function') throw new TypeError('at(when, fn): fn must be a function');
        const tick = current.tick, sec = current.sec, exact = current.exact;
        current.sec = t.sec;
        current.tick = t.sec * current.bpm / 60;
        current.exact = t.sample;
        try { fn(); } finally { current.tick = tick; current.sec = sec; current.exact = exact; }
    }

    // inputs: the ditty declares defaults, the caller's values win
    const inputValues = {};
    const declared = new Set();
    const input = new Proxy(inputValues, {
        set(target, k, v) {
            declared.add(k);
            target[k] = Object.prototype.hasOwnProperty.call(overrides, k) ? overrides[k] : v;
            return true;
        },
    });

    const debug = {
        log: (label, msg) => say('log', msg === undefined ? String(label) : `${label}: ${msg}`),
        warn: (label, msg) => say('warn', msg === undefined ? String(label) : `${label}: ${msg}`),
        error: (label, msg) => say('error', msg === undefined ? String(label) : `${label}: ${msg}`),
        probe() {},
    };
    const unsupportedEnv = (name) => ({ __env: name });

    // SN5 -- the synths a partition names. A name, a synth made with synth.def, and how to play it:
    // play(params, facts) turns an event's free parameters and what the partition says of it
    // (start, anchor, lead, length in seconds; the cut or the shot; beat, velocity) into play()
    // options; chain() gives the filters a track of it goes through.
    const instruments = new Map();
    function instrument(name, def, options = {}) {
        if (current) throw new Error('instrument() must be called at the top level');
        if (typeof name !== 'string' || !name) throw new TypeError('instrument(name, synth): name must be a non-empty string');
        if (!def || typeof def.play !== 'function') throw new TypeError(`instrument("${name}"): expected a synth made with synth.def`);
        if (instruments.has(name)) throw new Error(`instrument "${name}" is defined twice`);
        const { play, chain, note } = options;
        if (play !== undefined && typeof play !== 'function') throw new TypeError(`instrument("${name}"): play must be a function (params, facts) => options`);
        if (chain !== undefined && typeof chain !== 'function' && !Array.isArray(chain)) throw new TypeError(`instrument("${name}"): chain must be a function returning filters, or a list of shared filters`);
        instruments.set(name, { name, def, play, chain, note: note ?? 0 });
    }
    // Plays `fn` with the loop standing on an exact sample.
    function atSample(sample, fn) {
        const tick = current.tick, sec = current.sec, exact = current.exact;
        current.sec = sample / SR;
        current.tick = current.sec * current.bpm / 60;
        current.exact = sample;
        try { fn(); } finally { current.tick = tick; current.sec = sec; current.exact = exact; }
    }

    const sandbox = {
        ditty, synth, filter, loop, sleep, sleepUntil, at, input, debug, instrument,
        env: { def() { throw new Error('env.def is not supported by this renderer yet'); } },
        adsr: { __env: 'adsr' }, one: { __env: 'one' }, adsr2: unsupportedEnv('adsr2'), segenv: unsupportedEnv('segenv'),
        midi_to_hz, hz_to_midi, clamp, clamp01, lerp,
        tick_to_second: (t) => t * 60 / ditty.bpm,
        second_to_tick: (s) => s * ditty.bpm / 60,
        __random: () => rand(),
        __editJson: o.edit ? JSON.stringify(o.edit) : null,
        ...noteConstants(),
    };
    sandbox.sine = synth.def((ph, env) => Math.sin(ph * TAU) * env.value);

    const ctx = vm.createContext(sandbox, { codeGeneration: { strings: false, wasm: false } });
    vm.runInContext(PRELUDE, ctx, { filename: 'prelude.js' });
    try {
        vm.runInContext(o.code, ctx, { filename: o.filename ?? 'ditty.js', timeout: 10000 });
    } catch (e) {
        throw new Error(`ditty failed at the top level: ${e.message}`, { cause: e });
    }
    for (const k of Object.keys(overrides)) if (!declared.has(k)) say('warn', `input.${k} was set but the ditty does not declare it`);

    // SN5 -- the partition, one loop per track, after the ditty's own loops.
    if (instruments.size && !o.partition)
        say('warn', `the ditty defines instruments (${[...instruments.keys()].join(', ')}) but no partition plays them: give --partition`);
    let schedule = null;
    if (o.partition) {
        // Raw JSON carries its "format"; readPartition's output does not.
        schedule = schedulePartition(o.partition.format !== undefined ? readPartition(o.partition) : o.partition, o.edit, SR);
        for (const track of schedule) {
            const ins = instruments.get(track.instrument);
            if (!ins) {
                const known = [...instruments.keys()];
                throw new Error(`track "${track.name}" plays instrument "${track.instrument}", which the instruments do not define (${known.length ? known.join(', ') : 'none is defined: call instrument(name, synth)'})`);
            }
            const handle = loop(() => {
                for (const item of track.items) {
                    atSample(item.sample, () => {
                        const params = Object.freeze({ ...item.params });
                        const facts = Object.freeze({ ...item.facts, track: track.name });
                        let opts = ins.play ? ins.play(params, facts) : { ...params };
                        if (!opts || typeof opts !== 'object') throw new TypeError(`instrument "${ins.name}": play() must return play options, got ${opts}`);
                        opts = { ...opts };
                        if (opts.duration === undefined && facts.length !== undefined) opts.duration = facts.length * ditty.bpm / 60;
                        if (opts.amp === undefined && facts.velocity !== undefined) opts.amp = facts.velocity;
                        ins.def.play(item.note ?? ins.note, opts);
                    });
                }
            }, { name: track.name });
            loops[loops.length - 1].partition = track;
            const chain = typeof ins.chain === 'function' ? ins.chain() : ins.chain ?? [];
            if (!Array.isArray(chain)) throw new TypeError(`instrument "${ins.name}": chain() must return a list of filters`);
            for (const f of chain) handle.connect(f);
        }
    }

    // length: every frame of the edit, whole, then the tail
    const length = o.edit ? o.edit.duration : o.duration;
    if (!(length > 0)) throw new Error('nothing sets the render length: give an edit or a duration in seconds');
    const tail = Math.max(0, o.tail ?? 0);
    const N = o.edit && Number.isInteger(o.edit.frames)
        ? framesToSamplesCovering(o.edit.frames, o.edit.fps, SR) + Math.round(tail * SR)
        : Math.round((length + tail) * SR);
    const endSec = N / SR;

    // evaluate every loop up front onto the timeline (Dittytoy does the same)
    for (const L of loops) {
        current = L;
        L.bpm = globalBpm;
        rand = mulberry32(seedOf(seed, 1, L.index));
        let body = L.fn;
        if (L.isClass) { const obj = new L.fn(); body = (lc) => obj.process(lc); }
        let lc = 0;
        try {
            while (L.sec < endSec) {
                const start = { tick: L.tick, sec: L.sec, n: L.events.length };
                body(lc);
                if (L.opts.sync > 0) {
                    const syncSec = start.sec + L.opts.sync * 60 / L.bpm;
                    const kept = L.events.slice(start.n).filter((e) => e.sec < syncSec);
                    L.events.length = start.n;
                    L.events.push(...kept);
                    L.tick = start.tick + L.opts.sync;
                    L.sec = syncSec;
                    L.exact = undefined;
                }
                if (L.sec <= start.sec) {           // an iteration that does not sleep ends the loop
                    if (lc > 0) say('warn', `${L.opts.name}: an iteration did not sleep, the loop stopped at ${L.sec.toFixed(3)} s`);
                    break;
                }
                if (++lc > 1e6) throw new Error('more than a million iterations');
            }
        } catch (e) {
            throw new Error(`loop "${L.opts.name}" failed: ${e.message}`, { cause: e });
        } finally {
            current = null;
        }
        for (const e of L.events) e.sample = e.exact !== undefined ? e.exact : Math.round(e.sec * SR);
        const heard = L.events.filter((e) => e.sample < N);
        if (L.partition && heard.length < L.events.length)
            say('warn', `track "${L.opts.name}": ${L.events.length - heard.length} of its plays start after the end and are not heard`);
        L.events = heard;
        L.events.sort((x, y) => x.sample - y.sample || x.n - y.n);
    }

    // a voice that starts before 0 s (a whoosh leading into the first frame) is pre-rolled
    let n0 = 0;
    for (const L of loops) if (L.events.length) n0 = Math.min(n0, L.events[0].sample);

    // filters get their own random streams and are built once the tempo is known
    for (const f of filters) {
        f.rng = mulberry32(seedOf(seed, 3, f.index));
        rand = f.rng;
        f.ro = resolve(f.opts, 0);
        f.dyn = hasFunctions(f.opts);
        f.inst = f.def.isClass ? new f.def.impl(f.ro) : null;
        f.in = [0, 0];
    }
    const shared = new Map();
    for (const L of loops) {
        const last = L.chain[L.chain.length - 1];
        if (last && last.shared && !shared.has(last)) shared.set(last, [0, 0]);
        L.ro = resolve(L.opts, 0);
        L.dyn = hasFunctions(L.opts);
    }

    const left = new Float32Array(N), right = new Float32Array(N);
    const stems = o.stems ? loops.map(() => [new Float32Array(N), new Float32Array(N)]) : null;
    const active = loops.map(() => []);
    const ptr = loops.map(() => 0);
    const dt = 1 / SR;
    const ticksPerSample = globalBpm / 60 / SR;
    const secPerTick = 60 / globalBpm;

    const startVoice = (ev) => {
        const rng = mulberry32(seedOf(seed, 2, ev.loop, ev.n));
        rand = rng;
        const ro = resolve(ev.opts, 0);
        const kind = ro.env && ro.env.__env ? ro.env.__env : 'adsr';
        if (kind !== 'adsr' && kind !== 'one') throw new Error(`env ${kind} is not supported yet (adsr and one are)`);
        return {
            ev, ro, rng, t: 0, ph: 0, dyn: hasFunctions(ev.opts),
            e: makeEnv(kind, ro, globalBpm),
            inst: ev.def.isClass ? new ev.def.impl(ro) : null,
        };
    };

    for (let n = n0; n < N; n++) {
        renderSample = n;
        const refresh = (n & 31) === 0;
        const tickNow = n * ticksPerSample;
        for (const acc of shared.values()) { acc[0] = 0; acc[1] = 0; }

        for (let li = 0; li < loops.length; li++) {
            const L = loops[li], evs = L.events, vs = active[li];
            while (ptr[li] < evs.length && evs[ptr[li]].sample <= n) vs.push(startVoice(evs[ptr[li]++]));

            let l = 0, r = 0;
            for (let k = vs.length - 1; k >= 0; k--) {
                const v = vs[k];
                if (refresh && v.dyn) { rand = v.rng; v.ro = resolve(v.ev.opts, v.t); }
                v.e.update(v.t * secPerTick);
                if (v.e.done) { vs.splice(k, 1); continue; }
                rand = v.rng;
                const note = typeof v.ev.note === 'function' ? v.ev.note(v.t, v.ro) : v.ev.note;
                let y;
                if (v.inst) y = v.inst.process(note, v.e, v.t, v.ro);
                else { v.ph += midi_to_hz(note) * dt; y = v.ev.def.impl(v.ph, v.e, v.t, v.ro); }
                const amp = v.ro.amp, pan = v.ro.pan;
                const gl = pan > 0 ? 1 - pan : 1, gr = pan < 0 ? 1 + pan : 1;
                if (typeof y === 'number') { l += y * amp * gl; r += y * amp * gr; }
                else if (y && y.length === 2) { l += y[0] * amp * gl; r += y[1] * amp * gr; }
                else throw new Error(`a synth in "${L.opts.name}" returned ${y}; process() must return a number or [left, right]`);
                v.t += ticksPerSample;
            }

            if (refresh && L.dyn) L.ro = resolve(L.opts, tickNow);
            const lp = L.ro.pan;
            let x0 = l * L.ro.amp * (lp > 0 ? 1 - lp : 1), x1 = r * L.ro.amp * (lp < 0 ? 1 + lp : 1);
            let dest = null;
            for (const f of L.chain) {
                if (f.shared) { dest = shared.get(f); break; }
                rand = f.rng;
                if (refresh && f.dyn) f.ro = resolve(f.opts, tickNow);
                f.in[0] = x0; f.in[1] = x1;
                const y = f.inst ? f.inst.process(f.in, f.ro) : f.def.impl(f.in, f.ro);
                if (!y || y.length !== 2) throw new Error(`a filter in "${L.opts.name}" returned ${y}; process() must return [left, right]`);
                x0 = y[0]; x1 = y[1];
            }
            if (!Number.isFinite(x0) || !Number.isFinite(x1))
                throw new Error(`loop "${L.opts.name}" produced ${x0}/${x1} at ${(n / SR).toFixed(4)} s`);
            if (n >= 0 && stems) { stems[li][0][n] = x0; stems[li][1][n] = x1; }
            if (dest) { dest[0] += x0; dest[1] += x1; }
            else if (n >= 0) { left[n] += x0; right[n] += x1; }
        }

        for (const [f, acc] of shared) {
            rand = f.rng;
            if (refresh && f.dyn) f.ro = resolve(f.opts, tickNow);
            f.in[0] = acc[0]; f.in[1] = acc[1];
            const y = f.inst ? f.inst.process(f.in, f.ro) : f.def.impl(f.in, f.ro);
            if (!y || !Number.isFinite(y[0]) || !Number.isFinite(y[1])) throw new Error(`a shared filter produced ${y} at ${(n / SR).toFixed(4)} s`);
            if (n >= 0) { left[n] += y[0]; right[n] += y[1]; }
        }
    }

    return {
        left, right, sampleRate: SR, duration: endSec, seed,
        inputs: { ...inputValues },
        loops: loops.map((L) => ({
            name: L.opts.name, events: L.events.length,
            ...(L.partition ? { instrument: L.partition.instrument } : {}),
            // SN9 -- with each trigger, its note: what a partition and the ditty it replaces are
            // compared on (null when a voice's note is a function of time).
            ...(o.triggers ? { triggers: L.events.map((e) => e.sample), notes: L.events.map((e) => (typeof e.note === 'number' ? e.note : null)) } : {}),
        })),
        stems: stems ? loops.map((L, i) => ({ name: L.opts.name, left: stems[i][0], right: stems[i][1] })) : null,
        messages,
    };
}
