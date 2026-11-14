#!/usr/bin/env node
// cm-sound — render ditties against an edit, from the command line.
// Results are printed as JSON on stdout; errors as JSON on stderr with exit code 1.

import { readFileSync, writeFileSync } from 'node:fs';
import { basename, resolve } from 'node:path';
import { Worker } from 'node:worker_threads';
import { editOf } from './job.mjs';
import { formatPartition } from './partition.mjs';
import { readProject } from './project.mjs';

const MONTAGE = `(--edl <file.edl> [--fps <rate>] [--start <TC>] | --edit <edit.json>
                   | --project <project> [--timeline <id>] [--track <id>] [--start <TC>])`;
const USAGE = `usage:
  cm-sound render <ditty.js> ${MONTAGE.replace(')', ' | --duration <s>)')}
                  [--partition <partition.json>] [--out <file.wav>] [--stems <dir>] [--seed <n>]
                  [--set name=value]... [--sr <rate>] [--tail <s>] [--timeout <s>] [--triggers]
                  with --partition, the ditty is an instrument set (instrument(name, synth))
  cm-sound edit   ${MONTAGE}
                  prints the edit exactly as a ditty sees it in the \`edit\` global
  cm-sound strudel <pattern.js> (--cycles <n> | ${MONTAGE})
                  [--at <anchor JSON>] [--title <text>] [--out <partition.json>] [--strudel <dir>]
                  writes a Strudel pattern as a partition: --cycles of it, or, given an edit,
                  the cycles that reach its end from --at (the grid's origin, the edit's start)
  <project> is a .cutmachine-project folder or its project.cutmachine.json`;

const FLAGS = new Set(['triggers']);

function parseArgs(argv) {
    const args = { _: [], set: {} };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (!a.startsWith('--')) { args._.push(a); continue; }
        const key = a.slice(2);
        if (FLAGS.has(key)) { args[key] = true; continue; }
        const val = argv[i + 1];
        if (val === undefined || val.startsWith('--')) throw new Error(`--${key} needs a value`);
        i++;
        if (key === 'set') {
            const m = val.match(/^([A-Za-z_$][\w$]*)=(.*)$/);
            if (!m) throw new Error(`--set expects name=value, got "${val}"`);
            const num = Number(m[2]);
            args.set[m[1]] = m[2] !== '' && Number.isFinite(num) ? num : m[2];
        } else args[key] = val;
    }
    return args;
}

// The montage as data (job.mjs): files are read here, never inside the render.
function montageOf(args) {
    const job = {};
    if (args.edl) job.edl = { text: readFileSync(args.edl, 'utf8'), fps: args.fps, start: args.start };
    if (args.edit) job.edit = JSON.parse(readFileSync(args.edit, 'utf8'));
    if (args.project) job.project = { json: readProject(args.project), timeline: args.timeline, track: args.track, start: args.start };
    return job;
}

function fail(err) {
    process.stderr.write(JSON.stringify({ ok: false, error: err.message }) + '\n');
    process.exit(1);
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const [cmd, file] = args._;

    if (cmd === 'edit') {
        const edit = editOf(montageOf(args));
        if (!edit) throw new Error('edit needs --edl, --edit or --project');
        process.stdout.write(JSON.stringify(edit, null, 2) + '\n');
        return;
    }
    if (cmd === 'strudel' && file) {
        // SN9 -- Strudel is loaded only here: rendering never needs it.
        const { compileStrudel, loadStrudel, strudelLocation } = await import('./strudel.mjs');
        const strudel = await loadStrudel(strudelLocation(args.strudel));
        const edit = editOf(montageOf(args));
        const { partition, report } = await compileStrudel(strudel, readFileSync(file, 'utf8'), {
            cycles: args.cycles, edit, title: args.title, file: basename(file),
            at: args.at !== undefined ? JSON.parse(args.at) : undefined,
        });
        const out = resolve(args.out ?? basename(file).replace(/\.[^.]+$/, '') + '.partition.json');
        writeFileSync(out, formatPartition(partition));
        process.stdout.write(JSON.stringify({ ok: true, out, ...report }, null, 2) + '\n');
        return;
    }
    if (cmd !== 'render' || !file) throw new Error(USAGE);

    const job = {
        ...montageOf(args),
        code: readFileSync(file, 'utf8'),
        filename: basename(file),
        duration: args.duration !== undefined ? Number(args.duration) : undefined,
        sampleRate: args.sr !== undefined ? Number(args.sr) : 48000,
        seed: args.seed !== undefined ? Number(args.seed) : 1,
        inputs: args.set,
        tail: args.tail !== undefined ? Number(args.tail) : 0,
        stems: !!args.stems,
        partition: args.partition ? JSON.parse(readFileSync(args.partition, 'utf8')) : undefined,
        partitionFile: args.partition ? basename(args.partition) : undefined,
        triggers: !!args.triggers,
    };
    const files = {
        stemsDir: args.stems ? resolve(args.stems) : null,
        out: resolve(args.out ?? basename(file).replace(/\.[^.]+$/, '') + '.wav'),
    };
    const timeout = (args.timeout !== undefined ? Number(args.timeout) : 300) * 1000;

    const report = await new Promise((done, reject) => {
        const worker = new Worker(new URL('./worker.mjs', import.meta.url), { workerData: { job, ...files } });
        const timer = setTimeout(() => {
            worker.terminate();
            reject(new Error(`render stopped after ${timeout / 1000} s (--timeout)`));
        }, timeout);
        worker.once('message', (m) => { clearTimeout(timer); done(m); });
        worker.once('error', (e) => { clearTimeout(timer); reject(e); });
        worker.once('exit', (code) => { clearTimeout(timer); if (code !== 0) reject(new Error(`render worker exited with code ${code}`)); });
    });
    process.stdout.write(JSON.stringify(report, null, 2) + '\n');
}

main().catch(fail);
