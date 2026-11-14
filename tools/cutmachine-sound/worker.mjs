// Runs one render in a worker thread so the CLI can stop a ditty that never finishes.
// The render itself is job.mjs's, shared with the JavaScriptCore host: here, only the files and
// the hash, which are Node's.
import { parentPort, workerData } from 'node:worker_threads';
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { createHash } from 'node:crypto';
import { runJob } from './job.mjs';

const { job, out, stemsDir } = workerData;
const { report, main, stems } = runJob(job, { now: () => performance.now() });

mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, main.bytes);
const written = [];
if (stemsDir) {
    mkdirSync(stemsDir, { recursive: true });
    for (const s of stems) {
        const file = join(stemsDir, s.fileName);
        writeFileSync(file, s.wav.bytes);
        written.push({ name: s.name, file, peakDb: s.peakDb });
    }
}

parentPort.postMessage({
    ok: true,
    out,
    sha256: createHash('sha256').update(main.bytes.subarray(main.dataOffset)).digest('hex'),
    ...report,
    stems: written,
});
