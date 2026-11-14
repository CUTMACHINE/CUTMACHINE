#!/usr/bin/env node
// SN5 -- docs/roadmap/SON-2026-10.md. The partition impacts.js plays, written as data for any edit.
//
// The choices are impacts.js's, made here once by code and written down: an impact on every plain
// cut, weighted by the shots around it; an élan leading into some cuts after a long enough shot,
// drawn with the seed from the very stream impacts.js draws them from; a swell over each
// transition. Played with instruments/impacts.js, the partition renders what impacts.js renders,
// to the byte. An agent may then edit it -- move an impact, drop an élan -- without touching a
// time: every event names its cut.
//
//   node partitions/impacts.mjs --edl examples/demo.edl --fps 25 > impacts-demo.json
//   node partitions/impacts.mjs --project Film.cutmachine-project [--seed 1] [--elan 0.4]

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { editFromEdl } from '../edl.mjs';
import { editFromProject, readProject } from '../project.mjs';
import { PARTITION_FORMAT, formatPartition } from '../partition.mjs';
import { mulberry32, seedOf } from '../runtime.mjs';

const clamp = (t, lo, hi) => (t < lo ? lo : t > hi ? hi : t);

// A cut's weight, from 0.15 to 1: a long shot that breaks hits hard, a burst of short ones stays light.
export const poids = (c) => clamp(0.15 + 0.6 * Math.min(1, c.before / 3) + 0.25 * Math.min(1, c.after / 2), 0.15, 1);

/** impacts.js's choices on `edit`, as a partition. `elan` is the chance of an élan before a cut. */
export function impactsPartition(edit, { seed = 1, elan = 0.4 } = {}) {
    // impacts.js draws its élans in its second loop, whose stream is (seed, 1, loop index 1).
    const random = mulberry32(seedOf(seed, 1, 1));
    const impacts = [], elans = [], fondus = [];
    for (const c of edit.cuts) {
        if (c.type === 'cut') impacts.push({ at: { cut: c.id }, params: { poids: poids(c) } });
    }
    for (const c of edit.cuts) {
        if (c.type !== 'cut' || c.before < 0.8 || random() >= elan) continue;
        elans.push({ at: { cut: c.id }, lead: Math.min(0.8, c.before * 0.6), params: { poids: poids(c) } });
    }
    for (const c of edit.cuts) {
        if (c.type === 'cut' || c.transition <= 0) continue;
        fondus.push({ at: { cut: c.id }, length: 'transition' });
    }
    return {
        format: PARTITION_FORMAT,
        version: 1,
        title: `Impacts${edit.title ? ` — ${edit.title}` : ''}`,
        comment: `impacts.js's choices, seed ${seed}, élan ${elan}`,
        tracks: [
            { name: 'impacts', instrument: 'impact', events: impacts },
            { name: 'elans', instrument: 'elan', events: elans },
            { name: 'fondus', instrument: 'fondu', events: fondus },
        ],
    };
}

function main(argv) {
    const args = {};
    for (let i = 0; i < argv.length; i += 2) {
        if (!argv[i].startsWith('--') || argv[i + 1] === undefined) throw new Error(`expected --name value, got "${argv[i]}"`);
        args[argv[i].slice(2)] = argv[i + 1];
    }
    const edit = args.edl ? editFromEdl(readFileSync(args.edl, 'utf8'), { fps: args.fps, start: args.start })
        : args.project ? editFromProject(readProject(args.project), { timeline: args.timeline, track: args.track, start: args.start })
            : null;
    if (!edit) throw new Error('usage: impacts.mjs (--edl <file> --fps <rate> | --project <project> [--timeline <id>] [--track <id>]) [--seed <n>] [--elan <0..1>]');
    const partition = impactsPartition(edit, { seed: args.seed !== undefined ? Number(args.seed) : 1, elan: args.elan !== undefined ? Number(args.elan) : 0.4 });
    process.stdout.write(formatPartition(partition));
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
    try { main(process.argv.slice(2)); } catch (e) {
        process.stderr.write(JSON.stringify({ ok: false, error: e.message }) + '\n');
        process.exit(1);
    }
}
