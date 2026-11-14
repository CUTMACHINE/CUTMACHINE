// Runs every *.test.mjs of this folder in one process: what CTest and `npm test`
// call. `node --test <folder>` is no answer -- Node 24 reads its arguments as
// globs and fails on a folder, Node 18 reads no glob at all -- and a bare
// `node --test` would also pick this file up and run every test twice.
import { readdirSync } from 'node:fs';

const here = new URL('.', import.meta.url);
for (const file of readdirSync(here).filter((f) => f.endsWith('.test.mjs')).sort())
    await import(new URL(file, here));
