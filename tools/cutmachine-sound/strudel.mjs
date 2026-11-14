// SN9 -- docs/roadmap/SON-2026-10.md. A Strudel pattern written as a partition.
//
// Strudel (strudel.cc, TidalCycles in JavaScript) writes music as patterns -- "bd*2 [~ sd]",
// cycles, transformations -- and plays them through WebAudio, in real time, mostly with samples.
// Here Strudel only says what plays when: its pattern engine (core and mini, plain JavaScript) is
// asked for a pattern's events, and they are written as a partition (partition.mjs), which the
// instruments of a ditty play on the edit. The sound is the instruments', never Strudel's.
//
// Nothing is reckoned by hand. Strudel's time is exact (fraction.js): an event's cycle, times four,
// is the partition's beat, written "p/q"; the samples are rounded once, by the renderer, as for any
// partition. Its randomness (degradeBy, sometimes) is a function of the cycle: a pattern always
// writes the same partition. Given an edit, the module counts the cycles that reach its end.
//
// Strudel is AGPL-3.0-or-later, like CUTMACHINE. The build fetches it (CMakeLists.txt,
// cmake/StrudelModules.cmake); it is loaded only when a pattern is compiled, so the renderer, the
// partition and the JavaScriptCore host stay without a dependency.

import { existsSync, realpathSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { PARTITION_FORMAT, anchorTime, readPartition, schedulePartition } from './partition.mjs';
import { exact, frameTime, mul, sub } from './time.mjs';

/** A cycle is written as four beats: setcpm(bpm / 4) is the tempo strudel.cc writes for 4/4. */
export const BEATS_PER_CYCLE = 4;
/** strudel.cc's tempo when the code sets none: half a cycle per second, 120 beats a minute. */
export const DEFAULT_CPS = 0.5;

// What an event of Strudel says that the partition writes elsewhere than in `params`.
const PITCH_KEYS = ['note', 'freq'];
const LENGTH_KEYS = ['clip', 'legato'];

const RESERVED = new Set(('break case catch class const continue debugger default delete do else enum export extends ' +
    'false finally for function if implements import in instanceof interface let new null package private protected ' +
    'public return static super switch this throw true try typeof var void while with yield await arguments eval')
    .split(' '));

/** Where the Strudel sources are: an explicit folder, $CUTMACHINE_STRUDEL, or the repository's build. */
export function strudelLocation(explicit) {
    const dir = explicit || process.env.CUTMACHINE_STRUDEL
        || new URL('../../build/_deps/strudel-src', import.meta.url).pathname;
    if (!existsSync(join(dir, 'packages', 'core', 'pattern.mjs')))
        throw new Error(`no Strudel at ${dir}: configure the build (cmake -S . -B build) to fetch it, or give --strudel <folder of a Strudel checkout>`);
    return realpathSync(dir);
}

// Strudel greets the console when it loads, and code may log: none of it reaches stdout, where the
// CLI prints its report.
async function quietly(fn, heard = []) {
    const saved = { log: console.log, info: console.info, warn: console.warn, error: console.error, debug: console.debug };
    for (const k of Object.keys(saved)) console[k] = (...a) => heard.push(a.map(String).join(' '));
    try { return await fn(); } finally { Object.assign(console, saved); }
}

/** Strudel's pattern engine, loaded once from `dir` (strudelLocation's). */
export async function loadStrudel(dir) {
    const url = (p) => pathToFileURL(join(dir, 'packages', p)).href;
    let core, mini;
    try {
        [core, mini] = await quietly(async () => [await import(url('core/index.mjs')), await import(url('mini/index.mjs'))]);
    } catch (e) {
        throw new Error(`Strudel at ${dir} does not load (${e.message}): its imports are found by the links the build writes ` +
            'beside the fetched sources (cmake/StrudelModules.cmake), or by the node_modules of a checkout', { cause: e });
    }
    // On strudel.cc the transpiler reads "double-quoted" strings as mini-notation; without it,
    // every string is.
    mini.miniAllStrings();
    const scope = {};
    for (const m of [core, mini])
        for (const [k, v] of Object.entries(m)) if (/^[A-Za-z_$][\w$]*$/.test(k) && !RESERVED.has(k)) scope[k] = v;
    return { core, mini, scope, dir };
}

// ─────────────────────────────────────────── the code, as strudel.cc reads it

const LABEL = /^(_?)(\$|[A-Za-z_][\w]*):(?!:)(.*)$/;
const trimEnd = (text) => text.replace(/\s+$/, '').replace(/;$/, '');

// The code's patterns: blocks labelled `$:` or `name:` at the start of a line (`_$:` mutes one), the
// lines above the first label being ordinary statements; or, without a label, the last statement,
// which must be an expression -- what strudel.cc plays.
function splitCode(code, compiles) {
    const lines = code.split('\n');
    const starts = lines.map((l, i) => (LABEL.test(l) ? i : -1)).filter((i) => i >= 0);
    if (starts.length) {
        const blocks = starts.map((start, k) => {
            const [, mute, name, rest] = lines[start].match(LABEL);
            const text = [rest, ...lines.slice(start + 1, starts[k + 1] ?? lines.length)].join('\n');
            return { label: name === '$' ? null : name, muted: !!mute, line: start + 1, text: trimEnd(text) };
        });
        return { preamble: lines.slice(0, starts[0]).join('\n'), blocks: blocks.filter((b) => !b.muted), muted: blocks.filter((b) => b.muted).length };
    }
    // From the bottom, the first line at the start of a statement from which the rest is one
    // expression: the lines above it are the preamble.
    for (let i = lines.length - 1; i >= 0; i--) {
        if (!/^[^\s/)\]}.,]/.test(lines[i])) continue;
        const preamble = lines.slice(0, i).join('\n'), text = trimEnd(lines.slice(i).join('\n'));
        if (compiles(preamble, [text])) return { preamble, blocks: [{ label: null, muted: false, line: i + 1, text }], muted: 0 };
    }
    throw new Error('no pattern: end the code with the pattern to play (an expression), or label each pattern with "$:" at the start of its line');
}

const bodyOf = (preamble, texts) => `"use strict";\n${preamble}\nreturn [${texts.map((t) => `(\n${t}\n)`).join(',')}];`;

function evaluate(strudel, code) {
    const tempo = { cps: null, said: [] };
    const scope = {
        ...strudel.scope,
        setcpm: (cpm) => { tempo.cps = tempoValue(strudel, cpm, 'setcpm').div(60); },
        setcps: (cps) => { tempo.cps = tempoValue(strudel, cps, 'setcps'); },
        // Sounds are the instruments of the set: what a ditty plays has no sample bank.
        samples: () => { tempo.said.push('samples() ignored: the instruments of the set play every sound'); },
    };
    scope.setCpm = scope.setcpm;
    scope.setCps = scope.setcps;
    const names = Object.keys(scope);
    const compiles = (preamble, texts) => {
        try { Function(...names, bodyOf(preamble, texts)); return true; } catch { return false; }
    };
    const { preamble, blocks, muted } = splitCode(code, compiles);
    let fn;
    try { fn = Function(...names, bodyOf(preamble, blocks.map((b) => b.text))); } catch (e) {
        throw new Error(`the code does not parse: ${e.message}`, { cause: e });
    }
    let values;
    try { values = fn(...names.map((k) => scope[k])); } catch (e) {
        const missing = e instanceof ReferenceError || (e instanceof TypeError && /is not a function/.test(e.message));
        const hint = missing
            ? ' (this module loads Strudel\'s core and mini-notation; tonal, the sample banks and the REPL are not part of it)' : '';
        throw new Error(`the code failed: ${e.message}${hint}`, { cause: e });
    }
    const patterns = blocks.map((b, i) => {
        const p = values[i];
        if (!p || typeof p.queryArc !== 'function')
            throw new Error(`${b.label ? `"${b.label}:"` : `the pattern at line ${b.line}`} is not a pattern: ${typeof p === 'object' ? JSON.stringify(p) : String(p)}`);
        return { label: b.label, line: b.line, pattern: p };
    });
    return { patterns, tempo, muted };
}

function tempoValue(strudel, v, name) {
    if (typeof v !== 'number' || !(v > 0)) throw new Error(`${name}(${typeof v === 'number' ? v : '…'}): a tempo is one number above zero here; a tempo that changes is not written yet`);
    return strudel.core.Fraction(v);
}

// ─────────────────────────────────────────── events to notes

// A Strudel fraction as the partition writes it: a whole number, or a "p/q" text.
function written(f) {
    const text = f.toFraction();
    return /^-?\d+$/.test(text) ? Number(text) : text;
}

// The pitch Strudel's synths would play (superdough's getFrequencyFromValue): `freq` first, then
// `note`, a name ("eb4", octave 3 when it has none) or a MIDI number; `octave` moves either. No
// pitch: the instrument's own note.
function pitchOf(core, v, where) {
    let midi;
    if (typeof v.freq === 'number') midi = core.freqToMidi(v.freq);
    else if (typeof v.note === 'string') {
        try { midi = core.noteToMidi(v.note); } catch (e) { throw new Error(`${where}: ${e.message}`); }
    } else if (typeof v.note === 'number') midi = v.note;
    else if (v.note !== undefined || v.freq !== undefined) throw new Error(`${where}: ${JSON.stringify(v.note ?? v.freq)} is not a pitch`);
    if (midi !== undefined && typeof v.octave === 'number') midi += 12 * v.octave;
    if (midi !== undefined && !Number.isFinite(midi)) throw new Error(`${where}: the pitch is not a number`);
    return midi;
}

function paramsOf(v, usedOctave, where) {
    const params = {};
    for (const [k, x] of Object.entries(v)) {
        if (k === 's' || k === 'velocity' || PITCH_KEYS.includes(k) || LENGTH_KEYS.includes(k) || (k === 'octave' && usedOctave)) continue;
        if (x && typeof x === 'object' && typeof x.toFraction === 'function') params[k] = Number(x.valueOf());
        else if (typeof x === 'function' || typeof x === 'symbol' || typeof x === 'bigint') throw new Error(`${where}: "${k}" is not a value a partition can write`);
        else params[k] = x;
    }
    return params;
}

/**
 * Compile Strudel code into a partition. `strudel` is loadStrudel's; `cycles` how many cycles to
 * write (a number or "p/q"), or, given `edit`, the span from the grid's origin `at` (an anchor of
 * the partition format, the start of the edit by default) to its end.
 */
export async function compileStrudel(strudel, code, { cycles, edit, at, title, file } = {}) {
    const heard = [];
    const { patterns, tempo, muted } = await quietly(() => evaluate(strudel, code), heard);
    const { Fraction } = strudel.core;
    const cps = tempo.cps ?? Fraction(DEFAULT_CPS);
    const bpm = cps.mul(60 * BEATS_PER_CYCLE);

    let span;
    if (cycles !== undefined) {
        span = exact(cycles, 'cycles');
        if (span.n <= 0n) throw new Error('cycles must be more than zero');
    } else if (edit) {
        // From the grid's origin to the end of the edit, in cycles, exactly: what starts before the
        // end is written, whole; the renderer plays what of it falls inside.
        const rest = sub(frameTime(edit.frames, edit.fps), anchorTime(at ?? { edit: 'start' }, edit, 'at'));
        if (rest.n <= 0n) throw new Error('the grid starts at or after the end of the edit: nothing to write');
        span = mul(rest, exact(cps.toFraction(), 'cps'));
    } else throw new Error('say how much to write: --cycles <n>, or an edit (--edl, --edit or --project) to fill to its end');
    const end = Fraction(`${span.n}/${span.d}`);

    const tracks = new Map();
    const controls = {};
    for (const { label, line, pattern } of patterns) {
        let haps;
        try { haps = await quietly(() => pattern.queryArc(0, end), heard); } catch (e) {
            throw new Error(`${label ? `"${label}:"` : `the pattern at line ${line}`} failed when queried: ${e.message}`, { cause: e });
        }
        haps = haps.filter((h) => h.hasOnset()).sort((a, b) => a.whole.begin.compare(b.whole.begin));
        for (const h of haps) {
            const where = `${label ? `"${label}:"` : `line ${line}`}, cycle ${h.whole.begin.toFraction()}`;
            const v = h.value;
            if (!v || typeof v !== 'object' || Array.isArray(v))
                throw new Error(`${where}: the pattern gives ${JSON.stringify(v)}, not a sound: write s("…") or note("…")`);
            const note = pitchOf(strudel.core, v, where);
            if (v.s !== undefined && (typeof v.s !== 'string' || !v.s)) throw new Error(`${where}: ${JSON.stringify(v.s)} is not a sound name`);
            // superdough plays a triangle when an event has a pitch and no sound.
            const instrument = v.s ?? (note !== undefined ? 'triangle' : null);
            if (!instrument) throw new Error(`${where}: the event names no sound and no note: ${JSON.stringify(v)}`);
            const clip = v.clip ?? v.legato;
            if (clip !== undefined && !(typeof clip === 'number' && clip > 0)) throw new Error(`${where}: clip ${JSON.stringify(clip)} must be a number above zero`);
            if (v.velocity !== undefined && !(typeof v.velocity === 'number' && v.velocity >= 0)) throw new Error(`${where}: velocity must be a number >= 0`);
            let beats = h.whole.end.sub(h.whole.begin).mul(BEATS_PER_CYCLE);
            if (clip !== undefined) beats = beats.mul(Fraction(clip));
            const params = paramsOf(v, note !== undefined && typeof v.octave === 'number', where);
            for (const k of Object.keys(params)) controls[k] = (controls[k] ?? 0) + 1;
            const key = `${label ?? ''}\u0000${instrument}`;
            if (!tracks.has(key)) tracks.set(key, { label, instrument, notes: [] });
            tracks.get(key).notes.push({
                beat: written(h.whole.begin.mul(BEATS_PER_CYCLE)), ...(note !== undefined ? { note } : {}),
                beats: written(beats), ...(v.velocity !== undefined ? { velocity: v.velocity } : {}),
                ...(Object.keys(params).length ? { params } : {}),
            });
        }
    }
    if (!tracks.size) throw new Error(`the pattern plays nothing in ${end.toFraction()} cycles`);

    // A track per label and sound: a named label is the track's name, with the sound when the
    // label holds several; otherwise the sound names it.
    const perLabel = {};
    for (const t of tracks.values()) perLabel[t.label] = (perLabel[t.label] ?? 0) + 1;
    const names = new Set();
    const out = [...tracks.values()].map((t) => {
        let name = t.label ? (perLabel[t.label] > 1 ? `${t.label} ${t.instrument}` : t.label) : t.instrument;
        for (let k = 2; names.has(name); k++) name = `${t.label ?? t.instrument} ${k}`;
        names.add(name);
        return { name, instrument: t.instrument, notes: t.notes };
    });
    const partition = {
        format: PARTITION_FORMAT, version: 1,
        ...(title ? { title } : {}),
        comment: `written by cm-sound strudel${file ? ` from ${file}` : ''}: ${end.toFraction()} cycles of ${BEATS_PER_CYCLE} beats`,
        tempo: { bpm: written(bpm), ...(at ? { at } : {}) },
        tracks: out,
    };
    // What this module writes, the partition reads: a key or a value it refuses is a defect here.
    const read = readPartition(partition);
    if (edit) schedulePartition(read, edit, 48000);

    const messages = [...tempo.said, ...heard.map((text) => `the code said: ${text}`)];
    if (muted) messages.push(`${muted} muted pattern${muted > 1 ? 's' : ''} (_$:) not written`);
    if (!tempo.cps) messages.push(`no setcpm() nor setcps(): strudel.cc's tempo, ${DEFAULT_CPS} cycle a second (${written(bpm)} bpm)`);
    return {
        partition,
        report: {
            cycles: written(end), cps: written(cps), bpm: written(bpm), beatsPerCycle: BEATS_PER_CYCLE,
            tracks: out.map((t) => ({ name: t.name, instrument: t.instrument, notes: t.notes.length })),
            controls, messages, strudel: strudel.dir,
        },
    };
}
