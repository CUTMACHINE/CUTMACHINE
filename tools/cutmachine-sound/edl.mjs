// Timecode, CMX3600 EDL parsing, and the `edit` object that ditties read.
//
// Every time a ditty needs (cut times, shot lengths, transition centres) is computed here,
// so neither the ditty nor the agent writing it has to do timecode arithmetic.

const TC_RE = /^(\d{1,2}):(\d{2}):(\d{2})([:;.,])(\d{2,3})$/;

export function parseFps(value) {
    if (value && typeof value === 'object' && value.num > 0) return { num: value.num, den: value.den || 1 };
    if (value === undefined || value === null || value === '') throw new Error('frame rate missing: pass --fps (25, 24, 23.976, 29.97, 24000/1001...)');
    const s = String(value).trim();
    const m = s.match(/^(\d+)\s*\/\s*(\d+)$/);
    if (m) return { num: +m[1], den: +m[2] };
    const ntsc = { '23.976': 24000, '23.98': 24000, '29.97': 30000, '47.952': 48000, '59.94': 60000, '119.88': 120000 };
    if (ntsc[s]) return { num: ntsc[s], den: 1001 };
    const v = Number(s);
    if (Number.isInteger(v) && v > 0) return { num: v, den: 1 };
    throw new Error(`unsupported frame rate "${value}"`);
}

const nominal = (fps) => Math.round(fps.num / fps.den);

function dropPerMinute(fps) {
    const nom = nominal(fps);
    if (fps.den !== 1001 || (nom !== 30 && nom !== 60)) throw new Error('drop-frame timecode only exists at 29.97 and 59.94 fps');
    return nom === 30 ? 2 : 4;
}

export function tcToFrames(tc, fps, dropFrame = false) {
    const m = String(tc).trim().match(TC_RE);
    if (!m) throw new Error(`invalid timecode "${tc}"`);
    const [h, mi, s, sep, f] = [+m[1], +m[2], +m[3], m[4], +m[5]];
    const nom = nominal(fps);
    if (f >= nom) throw new Error(`invalid timecode "${tc}" at ${nom} fps`);
    let frames = ((h * 60 + mi) * 60 + s) * nom + f;
    if (dropFrame || sep === ';' || sep === ',') {
        const totalMin = h * 60 + mi;
        frames -= dropPerMinute(fps) * (totalMin - Math.floor(totalMin / 10));
    }
    return frames;
}

export function framesToTc(frames, fps, dropFrame = false) {
    const nom = nominal(fps);
    let f = frames;
    if (dropFrame) {
        const drop = dropPerMinute(fps);
        const per10 = nom * 600 - drop * 9, perMin = nom * 60 - drop;
        const d = Math.floor(f / per10), m = f % per10;
        f += drop * 9 * d + (m > drop ? drop * Math.floor((m - drop) / perMin) : 0);
    }
    const pad = (x) => String(x).padStart(2, '0');
    const secs = Math.floor(f / nom);
    return `${pad(Math.floor(secs / 3600))}:${pad(Math.floor(secs / 60) % 60)}:${pad(secs % 60)}${dropFrame ? ';' : ':'}${pad(f % nom)}`;
}

/** Parse a CMX3600 EDL into raw events (all tracks, timecodes as strings). */
export function parseEdl(text) {
    const out = { title: null, dropFrame: false, events: [] };
    let last = null;
    const lines = text.replace(/^﻿/, '').split(/\r?\n/);
    lines.forEach((raw, i) => {
        const line = raw.trim();
        if (!line) return;
        let m;
        if ((m = line.match(/^TITLE:\s*(.*)$/i))) { out.title = m[1].trim(); return; }
        if ((m = line.match(/^FCM:\s*(.*)$/i))) { out.dropFrame = /^DROP/i.test(m[1].trim()); return; }
        if (line.startsWith('*')) {
            if (!last) return;
            if ((m = line.match(/^\*\s*FROM CLIP NAME:\s*(.*)$/i))) last.fromName = m[1].trim();
            else if ((m = line.match(/^\*\s*TO CLIP NAME:\s*(.*)$/i))) last.toName = m[1].trim();
            return;
        }
        const tok = line.split(/\s+/);
        if (!/^\d+$/.test(tok[0]) || tok.length < 8) return;          // M2, SPLIT and other lines
        const tcs = tok.slice(-4);
        if (!tcs.every((t) => TC_RE.test(t))) return;
        const mid = tok.slice(3, -4);
        let transFrames = 0;
        if (mid.length > 1 && /^\d+$/.test(mid[mid.length - 1])) transFrames = +mid.pop();
        last = {
            event: +tok[0], reel: tok[1], track: tok[2], trans: mid.join('').toUpperCase() || 'C', transFrames,
            srcIn: tcs[0], srcOut: tcs[1], recIn: tcs[2], recOut: tcs[3], line: i + 1,
        };
        out.events.push(last);
    });
    return out;
}

const isVideo = (track) => /V/i.test(track) || track.toUpperCase() === 'B';

const transitionType = (t) => (t === 'D' ? 'dissolve' : t.startsWith('W') ? 'wipe' : t.startsWith('K') ? 'key' : 'cut');

/**
 * Build the edit object from shots expressed in frames.
 * shots: [{ startFrame, endFrame, id?, name?, reel?, transition?: 'cut'|'dissolve'|'wipe'|'key'|'dip', transitionFrames? }]
 * startFrame/endFrame are timeline frames; `start` is the frame where the edit (and the rendered stem) begins.
 *
 * Every instant is given twice: in seconds, rounded to the microsecond, to read and to reckon
 * with, and in frames from the start, exact (`frame`, `endFrame`, `centerFrame`, the last one
 * possibly a half frame). A ditty that hands a cut or a shot to at() or sleepUntil() is placed
 * from its frame, converted to samples once (time.mjs).
 *
 * Every shot has an `id`: the one it was given (a clip's ULID, for an edit read from a project),
 * or `shot-<n>` by position when its source has no identities (an EDL). A cut's `id` is its two
 * shots' ids, `<from>><to>`: it names that junction and no other, so that an anchor on a cut an
 * edit no longer has is refused, instead of landing on whatever cut took its place.
 */
export function buildEdit({ fps, dropFrame = false, shots, start, title = null }) {
    fps = parseFps(fps);
    if (!shots.length) throw new Error('the edit has no video shots');
    const list = [...shots].sort((a, b) => a.startFrame - b.startFrame);
    const ids = new Set();
    for (const s of list) {
        if (s.id === undefined || s.id === null) continue;
        if (ids.has(s.id)) throw new Error(`two shots have the id "${s.id}"`);
        ids.add(s.id);
    }
    const warnings = [];
    for (let i = 1; i < list.length; i++)
        if (list[i].startFrame < list[i - 1].endFrame)
            warnings.push(`shots ${i - 1} and ${i} overlap (multi-track EDL?): only one video layer is expected`);
    const origin = start ?? list[0].startFrame;
    const end = Math.max(...list.map((s) => s.endFrame));
    const sec = (frames) => frames * fps.den / fps.num;
    const r6 = (x) => Math.round(x * 1e6) / 1e6;
    const tc = (frame) => framesToTc(frame, fps, dropFrame);

    const outShots = list.map((s, i) => ({
        index: i,
        id: s.id ?? `shot-${i + 1}`,
        start: r6(sec(s.startFrame - origin)),
        end: r6(sec(s.endFrame - origin)),
        duration: r6(sec(s.endFrame - s.startFrame)),
        frame: s.startFrame - origin,
        frames: s.endFrame - s.startFrame,
        endFrame: s.endFrame - origin,
        timecode: tc(s.startFrame),
        name: s.name ?? null,
        reel: s.reel ?? null,
        ...(s.sourceIn ? { sourceIn: s.sourceIn, sourceOut: s.sourceOut } : {}),
    }));

    const cuts = [];
    for (let i = 1; i < list.length; i++) {
        const s = list[i], p = list[i - 1];
        const type = s.transition ?? 'cut';
        const tf = type === 'cut' ? 0 : (s.transitionFrames ?? 0);
        const time = sec(s.startFrame - origin);
        cuts.push({
            index: i - 1,
            id: `${outShots[i - 1].id}>${outShots[i].id}`,
            time: r6(time),
            frame: s.startFrame - origin,
            timecode: tc(s.startFrame),
            type,
            transition: r6(sec(tf)),
            transitionFrames: tf,
            center: r6(time + sec(tf) / 2),
            end: r6(time + sec(tf)),
            centerFrame: s.startFrame - origin + tf / 2,
            endFrame: s.startFrame - origin + tf,
            before: outShots[i - 1].duration,
            after: outShots[i].duration,
            gap: r6(sec(Math.max(0, s.startFrame - p.endFrame))),
            from: outShots[i - 1].name,
            to: outShots[i].name,
        });
    }

    return {
        title,
        fps: { num: fps.num, den: fps.den },
        frameRate: r6(fps.num / fps.den),
        dropFrame,
        start: { frame: origin, timecode: tc(origin) },
        duration: r6(sec(end - origin)),
        frames: end - origin,
        shots: outShots,
        cuts,
        warnings,
    };
}

/** EDL text → edit object. Only video events are used; audio events are ignored. */
export function editFromEdl(text, { fps, start } = {}) {
    const edl = parseEdl(text);
    const rate = parseFps(fps ?? (edl.dropFrame ? '29.97' : undefined));
    const df = edl.dropFrame;
    const shots = [];
    for (const e of edl.events) {
        if (!isVideo(e.track)) continue;
        const startFrame = tcToFrames(e.recIn, rate, df), endFrame = tcToFrames(e.recOut, rate, df);
        if (endFrame <= startFrame) continue;                          // outgoing half of a dissolve
        const type = transitionType(e.trans);
        shots.push({
            startFrame, endFrame, reel: e.reel,
            name: (type === 'cut' ? e.fromName : e.toName ?? e.fromName) ?? null,
            sourceIn: e.srcIn, sourceOut: e.srcOut,
            transition: type, transitionFrames: e.transFrames,
        });
    }
    return buildEdit({
        fps: rate, dropFrame: df, shots, title: edl.title,
        start: start ? tcToFrames(start, rate, df) : undefined,
    });
}

/**
 * JSON → edit object. Accepts an edit already produced by this module, or the simple form:
 * { "fps": "25", "start": "01:00:00:00", "shots": [ { "in": 0, "out": 100, "name": "A", "id": "a",
 *   "transition": { "type": "dissolve", "frames": 25 } } ] }   (in/out in frames from the start, id optional)
 */
export function editFromJson(obj) {
    if (obj && Array.isArray(obj.cuts) && Array.isArray(obj.shots) && obj.fps && typeof obj.duration === 'number') return obj;
    if (!obj || !Array.isArray(obj.shots)) throw new Error('edit JSON needs a "shots" array');
    const fps = parseFps(obj.fps);
    const df = !!obj.dropFrame;
    const origin = obj.start ? tcToFrames(obj.start, fps, df) : 0;
    const shots = obj.shots.map((s, i) => {
        if (!Number.isInteger(s.in) || !Number.isInteger(s.out) || s.out <= s.in)
            throw new Error(`shot ${i}: "in" and "out" must be whole frames with out > in`);
        if (s.id !== undefined && (typeof s.id !== 'string' || !s.id))
            throw new Error(`shot ${i}: "id" must be a non-empty string`);
        return {
            startFrame: origin + s.in, endFrame: origin + s.out, id: s.id, name: s.name ?? null, reel: s.reel ?? null,
            transition: s.transition?.type ?? 'cut', transitionFrames: s.transition?.frames ?? 0,
        };
    });
    return buildEdit({ fps, dropFrame: df, shots, start: origin, title: obj.title ?? null });
}
