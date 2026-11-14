// SN2 -- docs/roadmap/SON-2026-10.md. The edit a ditty reads, taken from a CUTMACHINE project
// instead of an EDL the engine wrote for it.
//
// The result is buildEdit's (edl.mjs), and the rules that decide it are the engine's own, so that
// a timeline rendered from its project and from `cutmachine --export-edl` is the same edit:
//   - only video tracks make shots; a hidden track, and a multicam angle that is not its group's
//     active one (Multicam.cc), show nothing and make none;
//   - a clip's place is read in whole sequence frames, exactly; a clip that does not sit on whole
//     frames is refused, never rounded;
//   - a transition sits where ResolveTransitionSpan (TransitionRender.cc) puts it: one of n frames
//     aligned "center" starts floor(n / 2) frames before the cut. As in the engine's EDL, the
//     incoming shot starts, and the outgoing one ends, where the transition starts, so that
//     `cut.time`, `center` and `end` mean what they mean for an EDL. A fade to black at the end of
//     the track is a dissolve into a black shot (reel BL), as the EDL writes it; a fade from black
//     at its start is no cut, and the edit says so in `warnings`. A dip through a colour between
//     two shots, which no EDL carries, is a cut of type `dip`;
//   - the timeline starts at 01:00:00:00 unless told otherwise, as the engine's EDL does
//     (kEdlDefaultStartTimecode, Edl.h): the document carries no start timecode. 29.97 and 59.94
//     count drop-frame, as there.
//
// When several video tracks hold clips, the cuts are those of the picture they compose, by the
// rule timeline_stats counts shots with (decided by the author on 22 October 2026, SN8): a cutaway on V2
// makes two cuts, a title in motion makes none and is listed in `edit.titles`. `track` reads one
// track's clips instead.

import { existsSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { buildEdit, parseFps, tcToFrames } from './edl.mjs';

export const DEFAULT_START = '01:00:00:00';

/** A project's JSON from its package folder (`*.cutmachine-project`) or its file, as the engine's CLI takes it. */
export function readProject(path) {
    let file = path;
    if (existsSync(path) && statSync(path).isDirectory()) {
        file = join(path, 'project.cutmachine.json');
        if (!existsSync(file)) throw new Error(`"${path}" is a folder without project.cutmachine.json`);
    }
    return JSON.parse(readFileSync(file, 'utf8'));
}

// A RationalTime ({value, rate}) in whole frames of `fps`, or null when it falls inside a frame.
function wholeFrames(time, fps, what) {
    if (!time || !Number.isSafeInteger(time.value) || !Number.isSafeInteger(time.rate) || time.rate <= 0)
        throw new Error(`${what} is not a time the document can hold: ${JSON.stringify(time)}`);
    const num = BigInt(time.value) * BigInt(fps.num), den = BigInt(time.rate) * BigInt(fps.den);
    return num % den === 0n ? Number(num / den) : null;
}

// The timeline the edit is read from: `id`, or the project's active one, or a document's only one.
function pickTimeline(project, id) {
    if (Array.isArray(project.timelines)) {
        const wanted = id ?? project.active_timeline_id;
        const found = project.timelines.find((t) => t.id === wanted);
        if (found) return found;
        const known = project.timelines.map((t) => `${t.id} (« ${t.name} »)`).join(', ');
        throw new Error(id ? `the project has no timeline "${id}"; it has ${known}` : `the project names no active timeline; pass --timeline, one of ${known}`);
    }
    if (project.sequence && typeof project.sequence === 'object') {
        if (id && project.sequence.id !== id) throw new Error(`the document's timeline is "${project.sequence.id}", not "${id}"`);
        return project.sequence;
    }
    throw new Error('not a CUTMACHINE project or document: no "timelines" and no "sequence"');
}

/**
 * Project (or document) JSON → edit object.
 * @param {object} project       project.cutmachine.json, or a document ({ sequence, library })
 * @param {object} [o]
 * @param {string} [o.timeline]  timeline id; the active one by default
 * @param {string} [o.track]     one video track whose clips make the cuts, instead of the composed picture
 * @param {string} [o.start]     timecode of the timeline's first frame, 01:00:00:00 by default
 */
export function editFromProject(project, { timeline, track, start } = {}) {
    const sequence = pickTimeline(project, timeline);
    const fps = parseFps(sequence.frame_rate);
    const dropFrame = fps.den === 1001 && (fps.num === 30000 || fps.num === 60000);
    const origin = tcToFrames(start ?? DEFAULT_START, fps, dropFrame);
    const library = new Map((project.library ?? []).map((m) => [m.id, m]));
    const compositions = new Map((project.compositions ?? []).map((c) => [c.id, c]));
    const warnings = [];

    // The video tracks, numbered V1 from the bottom as the window and the EDL number them.
    const videoTracks = (sequence.tracks ?? []).filter((t) => t.kind === 'video').sort((a, b) => a.index - b.index);
    const hiddenAngles = new Set();
    for (const group of sequence.multicam_groups ?? []) {
        if (!group.active_angle_id) continue;
        for (const angle of group.angles ?? []) if (angle.id !== group.active_angle_id) hiddenAngles.add(angle.clip_id);
    }
    const layers = videoTracks.map((t, i) => ({
        track: t, number: i + 1, visible: t.visible !== false,
        clips: (t.clips ?? []).filter((c) => !hiddenAngles.has(c.id)),
    }));

    // Each clip in whole frames of the timeline: a clip off the grid is refused, never rounded.
    const bounds = new Map();
    const place = (c) => {
        const at = wholeFrames(c.timeline_in, fps, `clip "${c.id}" timeline_in`);
        const length = wholeFrames(c.duration, fps, `clip "${c.id}" duration`);
        if (at === null || length === null || length <= 0)
            throw new Error(`clip "${c.id}" does not start and end on whole frames of the timeline; reading it would move the cut`);
        const name = c.composition_id ? compositions.get(c.composition_id)?.name ?? null : library.get(c.source_id)?.filename ?? null;
        bounds.set(c.id, { at, end: at + length });
        return { clip: c, cutIn: at, cutOut: at + length, startFrame: at, endFrame: at + length, id: c.id, name };
    };

    let pieces, rule, strict;
    if (track !== undefined) {
        // One track named by the caller: its clips are the shots, motion clips included.
        const layer = layers.find((l) => l.track.id === track);
        if (!layer) {
            const other = (sequence.tracks ?? []).find((t) => t.id === track);
            throw new Error(other ? `track "${track}" is not a video track (its kind is ${other.kind})` : `the timeline has no track "${track}"`);
        }
        if (!layer.visible) throw new Error(`track V${layer.number} ("${track}") is hidden: it shows no picture and makes no cut`);
        if (!layer.clips.length) throw new Error(`track V${layer.number} ("${track}") holds no clip: there is no cut to read`);
        pieces = layer.clips.map((c) => ({ ...place(c), layer })).sort((a, b) => a.startFrame - b.startFrame);
        rule = { rule: 'track', track: layer.track.id, trackNumber: layer.number };
        strict = true;
    } else {
        // SN8 -- the picture as it is composed, by timeline_stats's rule (TimelineStats.h): at every
        // instant the highest visible track that holds a clip there is seen, and a motion clip (a
        // title, a name on screen) is drawn over the shots, neither a shot nor a cut. A cutaway
        // on V2 thus makes two cuts, a title on V2 none. A V1 clip a cutaway interrupts is seen
        // twice: its id on the first stretch, then `<id>~2`, `<id>~3`.
        const pictures = [];
        for (const l of layers) if (l.visible) for (const c of l.clips) if (!c.composition_id) pictures.push({ ...place(c), layer: l });
        if (!pictures.length) throw new Error('the timeline has no picture clip: there is no cut to read (motion clips are titles, not shots)');
        const edges = [...new Set(pictures.flatMap((p) => [p.cutIn, p.cutOut]))].sort((a, b) => a - b);
        pieces = [];
        const seen = new Map();
        for (let i = 0; i + 1 < edges.length; i++) {
            const [a, b] = [edges[i], edges[i + 1]];
            let top = null;
            for (const p of pictures)
                if (p.cutIn <= a && p.cutOut > a && (!top || p.layer.track.index > top.layer.track.index)) top = p;
            if (!top) continue;
            const last = pieces[pieces.length - 1];
            if (last && last.clip === top.clip && last.cutOut === a) { last.cutOut = b; last.endFrame = b; continue; }
            const n = (seen.get(top.clip.id) ?? 0) + 1;
            seen.set(top.clip.id, n);
            pieces.push({ ...top, id: n === 1 ? top.clip.id : `${top.clip.id}~${n}`, cutIn: a, cutOut: b, startFrame: a, endFrame: b });
        }
        rule = { rule: 'composite', track: null, trackNumber: null };
        strict = false;
    }
    for (const l of layers)
        if (!l.visible && l.clips.length) warnings.push(`track V${l.number} is hidden: its ${l.clips.length} clips make no shot`);

    const extra = [];
    const used = new Set(pieces.map((p) => p.clip.id));
    for (const t of sequence.transitions ?? []) {
        const layer = layers.find((l) => l.track.id === t.track_id);
        if (!layer || !layer.visible) continue;
        if (strict && layer.track.id !== rule.track) continue;
        const what = `transition "${t.id}"`;
        for (const id of [t.left_clip_id, t.right_clip_id]) if (id && !bounds.has(id) && !layer.clips.some((c) => c.id === id)) throw new Error(`${what} names a clip that is not on track V${layer.number}`);
        for (const id of [t.left_clip_id, t.right_clip_id]) if (id && !bounds.has(id)) place(layer.clips.find((c) => c.id === id));
        if (!t.left_clip_id && !t.right_clip_id) throw new Error(`${what} names no clip`);
        const cut = t.right_clip_id ? bounds.get(t.right_clip_id).at : bounds.get(t.left_clip_id).end;
        if (t.left_clip_id && t.right_clip_id && bounds.get(t.left_clip_id).end !== cut) throw new Error(`${what}: its clips do not share a cut`);
        const frames = wholeFrames(t.duration, fps, `${what} duration`);
        if (frames === null || frames <= 0) throw new Error(`${what} does not last a whole number of frames`);
        const alignment = t.alignment ?? 'center';
        const pre = alignment === 'center' ? Math.floor(frames / 2) : alignment === 'start_at_cut' ? 0 : alignment === 'end_at_cut' ? frames : NaN;
        if (Number.isNaN(pre)) throw new Error(`${what} has an alignment the engine does not know: "${alignment}"`);
        const first = cut - pre;
        const color = t.color ?? '#000000';
        // The stretches the transition joins, where the picture shows them meeting at its cut.
        const left = t.left_clip_id ? pieces.findIndex((p) => p.clip.id === t.left_clip_id && p.cutOut === cut) : -1;
        const right = t.right_clip_id ? pieces.findIndex((p) => p.clip.id === t.right_clip_id && p.cutIn === cut) : -1;
        const hidden = () => warnings.push(`${what} is covered by a higher track: it is not seen, and makes no cut`);
        if (t.left_clip_id && t.right_clip_id) {
            if (left < 0 || right !== left + 1) {
                if (strict) throw new Error(`${what} joins clips that are not adjacent`);
                hidden();
                continue;
            }
            // A dip through a colour between two shots has no EDL spelling: it keeps its own type.
            const type = (t.type ?? 'cross_dissolve') === 'dip_to_color' ? 'dip' : 'dissolve';
            pieces[left].endFrame = first;
            Object.assign(pieces[right], { startFrame: first, transition: type, transitionFrames: frames });
        } else if (t.right_clip_id) {
            if (right < 0) { hidden(); continue; }
            warnings.push(`${what} fades the first shot in from ${color}: no cut, the edit does not carry it`);
        } else {
            if (left < 0 || pieces.some((p) => p.cutIn === cut)) { hidden(); continue; }
            // A closing fade, as the EDL writes it: the shot ends where the fade starts, and a
            // dissolve leads into a shot of the colour (reel BL for black).
            pieces[left].endFrame = first;
            const black = color.toLowerCase() === '#000000';
            extra.push({ startFrame: first, endFrame: first + frames, id: t.id, name: black ? null : color,
                reel: black ? 'BL' : null, transition: 'dissolve', transitionFrames: frames });
        }
    }

    const shots = [...pieces, ...extra].map(({ clip, layer, cutIn, cutOut, ...s }) => ({ ...s, startFrame: origin + s.startFrame, endFrame: origin + s.endFrame }));
    for (const s of shots)
        if (s.endFrame <= s.startFrame) throw new Error(`shot "${s.id}" is all transition: nothing of it is seen on its own`);
    const edit = buildEdit({ fps, dropFrame, shots, start: origin, title: sequence.name ?? null });
    edit.timeline = { id: sequence.id, name: sequence.name ?? null, ...rule };

    // SN8 -- the titles: motion clips on visible tracks that are not shots, in the edit's frames.
    const sec = (frames) => Math.round(frames * fps.den / fps.num * 1e6) / 1e6;
    edit.titles = layers.filter((l) => l.visible).flatMap((l) => l.clips
        .filter((c) => c.composition_id && !used.has(c.id))
        .map((c) => {
            const { at, end } = bounds.get(c.id) ?? (place(c), bounds.get(c.id));
            return {
                id: c.id, name: compositions.get(c.composition_id)?.name ?? null, trackNumber: l.number,
                frame: at, endFrame: end, frames: end - at, start: sec(at), end: sec(end), duration: sec(end - at),
            };
        }))
        .sort((a, b) => a.frame - b.frame || a.trackNumber - b.trackNumber);
    edit.warnings.push(...warnings);
    return edit;
}
