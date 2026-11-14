// SN6 -- docs/roadmap/SON-2026-10.md. PROTOTYPE, not production code: the runtime as one classic
// script for a JSContext (JavaScriptCore), which has no node:vm, no worker and no module loader.
//
// The runtime is plain JavaScript but for node:vm. Here vm.runInContext becomes a Function whose
// parameters are the sandbox's names: the ditty sees the same globals, in the same realm as the
// runtime (Math.random and Array.prototype are then shared -- which node:vm kept apart, and which
// is no security boundary either way). Everything else is the modules' own text, imports and
// exports removed, so that what JSC measures is the code Node runs.
//
//   node jsc/bundle.mjs > runtime-jsc.js

import { readFileSync } from 'node:fs';

const here = (p) => new URL(`../${p}`, import.meta.url);
const strip = (text) => text
    .split('\n')
    .filter((line) => !/^import\s/.test(line) && !/^export\s*\{/.test(line))
    .map((line) => line.replace(/^export\s+(?=(async\s+)?(function|const|let|class)\b)/, ''))
    .join('\n');

const VM_SHIM = `
// node:vm, as far as runtime.mjs uses it: a context is its sandbox, and code runs with the
// sandbox's names as the parameters of a function.
const vm = {
    createContext(sandbox) { return sandbox; },
    runInContext(code, ctx) {
        const names = Object.keys(ctx).filter((k) => /^[A-Za-z_$][\\w$]*$/.test(k));
        return Function(...names, code)(...names.map((k) => ctx[k]));
    },
};
`;

// What the host calls: one render, and an FNV-1a over the samples as the WAV would write them
// (24 bits, clipped), to compare engines without a file.
const ENTRY = `
function fnv24(left, right) {
    let h = 0x811c9dc5;
    for (let i = 0; i < left.length; i++)
        for (const ch of [left, right]) {
            let x = ch[i];
            if (x > 1) x = 1; else if (x < -1) x = -1;
            const v = Math.max(-8388608, Math.min(8388607, Math.round(x * 8388607)));
            for (let b = 0; b < 3; b++) { h ^= (v >> (8 * b)) & 0xff; h = Math.imul(h, 0x01000193) >>> 0; }
        }
    return h >>> 0;
}
globalThis.benchRender = function (job) {
    const o = JSON.parse(job);
    o.code = o.code;
    const t0 = Date.now();
    const r = render(o);
    const ms = Date.now() - t0;
    return JSON.stringify({ seconds: r.left.length / r.sampleRate, ms, fnv: fnv24(r.left, r.right), messages: r.messages });
};
globalThis.benchSamples = null;
`;

const parts = ['time.mjs', 'edl.mjs', 'partition.mjs', 'runtime.mjs'].map((f) => `// ── ${f}\n${strip(readFileSync(here(f), 'utf8'))}`);
process.stdout.write(`'use strict';\n${VM_SHIM}\n${parts.join('\n')}\n${ENTRY}`);
