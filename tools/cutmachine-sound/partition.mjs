// SN5 -- docs/roadmap/SON-2026-10.md. A partition: what plays when, as data, apart from how it
// sounds.
//
// A ditty mixes the notes and the synthesis: keeping the one while changing the other meant
// rewriting it. A partition is the first half as JSON -- tracks, each naming an instrument, holding
// events anchored to the edit and notes on a tempo grid -- and an instrument set is the second
// half, as a ditty (runtime.mjs, instrument()). Played with another set, the same partition
// triggers on the same samples: what sounds changes, never when.
//
// Nothing here reckons a time by hand. An event names a cut or a shot by its id (edl.mjs), never a
// second; a note names a beat. The instants are exact fractions -- the anchor's frame, plus the
// beats and the seconds the partition chooses (a lead, an offset, a length) -- rounded into
// samples once (time.mjs). An id the edit no longer has is refused by name: a partition written
// for another cut of the film never lands on whatever cut took its place.
//
//   { "format": "cutmachine-partition", "version": 1,
//     "tempo": { "bpm": 96, "at": { "cut": "A>B" } },
//     "tracks": [
//       { "name": "impacts", "instrument": "impact",
//         "events": [ { "at": { "cut": "A>B" }, "params": { "poids": 0.8 } } ] },
//       { "name": "basse", "instrument": "basse", "params": { "drive": 0.3 },
//         "notes": [ { "beat": 0, "note": "c2", "beats": 1.5, "velocity": 0.9 } ] } ] }
//
// An anchor: { "cut": id, "point": "start" | "center" | "end" } (the transition's), or
// { "shot": id, "point": "start" | "center" | "end" }, or { "edit": "start" | "end" }; any of them
// with "offset" in seconds. An event: "at" (an anchor), "lead" (it starts that long before),
// "length" (seconds, "transition", "shot" or an anchor where it ends), "note", "params". A note:
// "beat" (from the grid's origin, "p/q" for a triplet), "note" (a MIDI number or a name, c4 = 60;
// without it, the instrument's own note, as for an event: a drum hit has no pitch -- SN9),
// "beats", "velocity", "params". Seconds and beats are numbers or "p/q" texts. "comment" is
// allowed at every level, and every other key is refused: a typo is not ignored.

import { add, div, exact, frameTime, mul, rational, sub, toNumber, toSample } from './time.mjs';

export const PARTITION_FORMAT = 'cutmachine-partition';

const NOTE_NAMES = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };

function fail(path, message) {
    throw new Error(`partition ${path}: ${message}`);
}

function keysOf(obj, path, allowed) {
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) fail(path, 'must be an object');
    for (const k of Object.keys(obj)) if (!allowed.includes(k) && k !== 'comment') fail(`${path}.${k}`, `is not a key of the format (${allowed.join(', ')})`);
}

/** A note as a MIDI number: 48, "c3", "f#2", "eb4", "cs4" (the ditty constants' spelling). */
export function noteNumber(value, path) {
    if (typeof value === 'number') {
        if (!Number.isFinite(value)) fail(path, `${value} is not a note`);
        return value;
    }
    const m = typeof value === 'string' && value.trim().toLowerCase().match(/^([a-g])(#|s|b)?(-?\d)$/);
    if (!m) fail(path, `${JSON.stringify(value)} is not a note (a MIDI number, or a name such as "c3", "f#2", "eb4")`);
    const shift = m[2] === '#' || m[2] === 's' ? 1 : m[2] === 'b' ? -1 : 0;
    return 12 * (Number(m[3]) + 1) + NOTE_NAMES[m[1]] + shift;
}

function seconds(value, path, { positive = false, nonNegative = false } = {}) {
    let r;
    try { r = exact(value, path); } catch (e) { fail(path, e.message); }
    if (positive && r.n <= 0n) fail(path, 'must be more than zero');
    if (nonNegative && r.n < 0n) fail(path, 'cannot be negative');
    return r;
}

function readAnchor(anchor, path) {
    keysOf(anchor, path, ['cut', 'shot', 'edit', 'point', 'offset']);
    const kinds = ['cut', 'shot', 'edit'].filter((k) => anchor[k] !== undefined);
    if (kinds.length !== 1) fail(path, 'names one of "cut", "shot" or "edit"');
    const kind = kinds[0];
    const target = anchor[kind];
    if (kind === 'edit') {
        if (target !== 'start' && target !== 'end') fail(`${path}.edit`, 'is "start" or "end"');
        if (anchor.point !== undefined) fail(`${path}.point`, 'does not apply to "edit"');
    } else {
        if (typeof target !== 'string' || !target) fail(`${path}.${kind}`, 'must be an id of the edit');
        if (anchor.point !== undefined && !['start', 'center', 'end'].includes(anchor.point)) fail(`${path}.point`, 'is "start", "center" or "end"');
    }
    return {
        kind, target, point: anchor.point ?? (kind === 'edit' ? null : 'start'),
        offset: anchor.offset === undefined ? null : seconds(anchor.offset, `${path}.offset`),
    };
}

function readTempo(tempo, path) {
    keysOf(tempo, path, ['bpm', 'at']);
    if (tempo.bpm === undefined) fail(`${path}.bpm`, 'is required');
    return {
        bpm: seconds(tempo.bpm, `${path}.bpm`, { positive: true }),
        at: tempo.at === undefined ? { kind: 'edit', target: 'start', point: null, offset: null } : readAnchor(tempo.at, `${path}.at`),
    };
}

function readParams(params, path) {
    if (params === undefined) return {};
    if (!params || typeof params !== 'object' || Array.isArray(params)) fail(path, 'must be an object of free parameters');
    return params;
}

/** Check a partition and give it back normalised. Throws with the path of what is wrong. */
export function readPartition(obj) {
    keysOf(obj, '', ['format', 'version', 'title', 'tempo', 'tracks']);
    if (obj.format !== PARTITION_FORMAT) fail('.format', `must be "${PARTITION_FORMAT}"`);
    if (obj.version !== 1) fail('.version', 'must be 1, the only version this renderer reads');
    if (!Array.isArray(obj.tracks) || !obj.tracks.length) fail('.tracks', 'must list at least one track');
    const tempo = obj.tempo === undefined ? null : readTempo(obj.tempo, '.tempo');
    const names = new Set();
    const tracks = obj.tracks.map((t, i) => {
        const path = `.tracks[${i}]`;
        keysOf(t, path, ['name', 'instrument', 'params', 'tempo', 'events', 'notes']);
        if (typeof t.instrument !== 'string' || !t.instrument) fail(`${path}.instrument`, 'must name an instrument of the set');
        const name = t.name ?? `${t.instrument} ${i + 1}`;
        if (typeof name !== 'string' || !name) fail(`${path}.name`, 'must be a non-empty string');
        if (names.has(name)) fail(`${path}.name`, `"${name}" names two tracks`);
        names.add(name);
        const trackTempo = t.tempo === undefined ? tempo : readTempo(t.tempo, `${path}.tempo`);
        const events = (t.events ?? []).map((e, k) => {
            const at = `${path}.events[${k}]`;
            keysOf(e, at, ['at', 'lead', 'length', 'note', 'params']);
            if (e.at === undefined) fail(`${at}.at`, 'is required: the cut, the shot or the edit point it plays on');
            let length = null;
            if (e.length !== undefined) {
                if (e.length === 'transition' || e.length === 'shot') length = { kind: e.length };
                else if (typeof e.length === 'object') length = { kind: 'until', anchor: readAnchor(e.length, `${at}.length`) };
                else length = { kind: 'seconds', value: seconds(e.length, `${at}.length`, { positive: true }) };
            }
            return {
                kind: 'event', index: k, path: at, anchor: readAnchor(e.at, `${at}.at`),
                lead: e.lead === undefined ? rational(0n) : seconds(e.lead, `${at}.lead`, { nonNegative: true }),
                length, note: e.note === undefined ? undefined : noteNumber(e.note, `${at}.note`),
                params: readParams(e.params, `${at}.params`),
            };
        });
        if (t.events !== undefined && !Array.isArray(t.events)) fail(`${path}.events`, 'must be a list');
        if (t.notes !== undefined && !Array.isArray(t.notes)) fail(`${path}.notes`, 'must be a list');
        const notes = (t.notes ?? []).map((n, k) => {
            const at = `${path}.notes[${k}]`;
            keysOf(n, at, ['beat', 'note', 'beats', 'velocity', 'params']);
            if (!trackTempo) fail(at, 'a note needs a tempo: give "tempo" to the partition or to its track');
            if (n.beat === undefined) fail(`${at}.beat`, 'is required');
            if (n.velocity !== undefined && !(typeof n.velocity === 'number' && n.velocity >= 0)) fail(`${at}.velocity`, 'must be a number >= 0');
            return {
                kind: 'note', index: k, path: at, beat: seconds(n.beat, `${at}.beat`),
                beats: n.beats === undefined ? null : seconds(n.beats, `${at}.beats`, { positive: true }),
                note: n.note === undefined ? undefined : noteNumber(n.note, `${at}.note`), velocity: n.velocity ?? 1,
                params: readParams(n.params, `${at}.params`),
            };
        });
        return { name, instrument: t.instrument, params: readParams(t.params, `${path}.params`), tempo: trackTempo, events, notes };
    });
    return { title: obj.title ?? null, tempo, tracks };
}

// The edit's object an anchor names, and its instant as an exact time.
function resolveAnchor(anchor, edit, path) {
    let frame, cut = null, shot = null;
    if (anchor.kind === 'edit') {
        frame = anchor.target === 'start' ? 0 : edit.frames;
    } else if (anchor.kind === 'cut') {
        cut = edit.cuts.find((c) => c.id === anchor.target);
        if (!cut) {
            const isShot = edit.shots.some((s) => s.id === anchor.target);
            fail(path, isShot ? `"${anchor.target}" is a shot, not a cut: write { "shot": "${anchor.target}" }`
                : `the edit has no cut "${anchor.target}" (cm-sound edit lists its cuts${edit.cuts.length ? `, such as "${edit.cuts[0].id}"` : ''})`);
        }
        frame = anchor.point === 'center' ? cut.centerFrame : anchor.point === 'end' ? cut.endFrame : cut.frame;
    } else {
        shot = edit.shots.find((s) => s.id === anchor.target);
        if (!shot) {
            const isCut = edit.cuts.some((c) => c.id === anchor.target);
            fail(path, isCut ? `"${anchor.target}" is a cut, not a shot: write { "cut": "${anchor.target}" }`
                : `the edit has no shot "${anchor.target}" (cm-sound edit lists its shots${edit.shots.length ? `, such as "${edit.shots[0].id}"` : ''})`);
        }
        frame = anchor.point === 'center' ? (shot.frame + shot.endFrame) / 2 : anchor.point === 'end' ? shot.endFrame : shot.frame;
    }
    let time = frameTime(frame, edit.fps);
    if (anchor.offset) time = add(time, anchor.offset);
    return { time, cut, shot };
}

/** The exact time, in seconds from the start of the edit, of an anchor as a partition writes it. */
export function anchorTime(anchor, edit, path = 'anchor') {
    return resolveAnchor(readAnchor(anchor, path), edit, path).time;
}

/**
 * Every event and note of `partition` (readPartition's) at its exact time and sample, track by
 * track, in the order the partition writes them. `facts` is what an instrument is told of each.
 */
export function schedulePartition(partition, edit, sampleRate) {
    if (!edit) throw new Error('a partition plays on an edit: give --edl, --edit or --project');
    const MINUTE = rational(60n);
    return partition.tracks.map((track) => {
        const items = [];
        for (const e of track.events) {
            const anchor = resolveAnchor(e.anchor, edit, `${e.path}.at`);
            const start = sub(anchor.time, e.lead);
            let length = null;
            if (e.length?.kind === 'seconds') length = e.length.value;
            else if (e.length?.kind === 'transition') {
                if (!anchor.cut) fail(`${e.path}.length`, '"transition" needs an event anchored on a cut');
                if (anchor.cut.endFrame === anchor.cut.frame) fail(`${e.path}.length`, `cut "${anchor.cut.id}" has no transition`);
                length = sub(frameTime(anchor.cut.endFrame, edit.fps), frameTime(anchor.cut.frame, edit.fps));
            } else if (e.length?.kind === 'shot') {
                if (!anchor.shot) fail(`${e.path}.length`, '"shot" needs an event anchored on a shot');
                length = sub(frameTime(anchor.shot.endFrame, edit.fps), frameTime(anchor.shot.frame, edit.fps));
            } else if (e.length?.kind === 'until') {
                length = sub(resolveAnchor(e.length.anchor, edit, `${e.path}.length`).time, start);
                if (length.n <= 0n) fail(`${e.path}.length`, 'ends before the event starts');
            }
            items.push({
                kind: 'event', path: e.path, start, sample: toSample(start, sampleRate), note: e.note,
                params: { ...track.params, ...e.params },
                facts: {
                    kind: 'event', index: e.index, start: toNumber(start), anchor: toNumber(anchor.time),
                    lead: toNumber(e.lead), length: length ? toNumber(length) : undefined,
                    cut: anchor.cut, shot: anchor.shot,
                },
            });
        }
        if (track.notes.length) {
            const origin = resolveAnchor(track.tempo.at, edit, '.tempo.at').time;
            const beat = div(MINUTE, track.tempo.bpm);                       // seconds per beat
            for (const n of track.notes) {
                const start = add(origin, mul(n.beat, beat));
                const length = n.beats ? mul(n.beats, beat) : null;
                items.push({
                    kind: 'note', path: n.path, start, sample: toSample(start, sampleRate), note: n.note,
                    params: { ...track.params, ...n.params },
                    facts: {
                        kind: 'note', index: n.index, start: toNumber(start), beat: toNumber(n.beat),
                        beats: n.beats ? toNumber(n.beats) : undefined, length: length ? toNumber(length) : undefined,
                        velocity: n.velocity, bpm: toNumber(track.tempo.bpm),
                    },
                });
            }
        }
        return { name: track.name, instrument: track.instrument, items };
    });
}

/** A partition as JSON a person reads: one event or note per line. */
export function formatPartition(partition) {
    const line = (x) => JSON.stringify(x);
    const list = (items) => (items.length ? `[\n${items.map((x) => `        ${line(x)}`).join(',\n')}\n      ]` : '[]');
    const tracks = partition.tracks.map((t) => {
        const fields = Object.entries(t).map(([k, v]) => `"${k}": ${k === 'events' || k === 'notes' ? list(v) : line(v)}`);
        return `    {\n      ${fields.join(',\n      ')}\n    }`;
    });
    const head = Object.entries(partition).filter(([k]) => k !== 'tracks').map(([k, v]) => `  "${k}": ${line(v)}`);
    return `{\n${[...head, `  "tracks": [\n${tracks.join(',\n')}\n  ]`].join(',\n')}\n}\n`;
}

