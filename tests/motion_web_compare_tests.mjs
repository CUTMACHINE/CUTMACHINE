// FI5 -- ROADMAP.md. Invalid inputs must fail even on a host without the
// browser dependencies; the report must explain the failure.
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const work = fs.mkdtempSync(path.join(os.tmpdir(), "cutmachine-compare-test-"));
try {
  const registry = path.join(work, "registry");
  const pieces = path.join(work, "pieces");
  const out = path.join(work, "out");
  fs.mkdirSync(registry);
  fs.mkdirSync(pieces);
  const run = () => {
    const result = spawnSync(process.execPath, [path.join(root, "tools/motion-web-import/compare.mjs"),
      registry, path.join(work, "no-modules"), pieces, out, "test"],
    { encoding: "utf8", env: { ...process.env, PLAYWRIGHT_MODULE: "missing-playwright-for-test" } });
    assert.equal(result.status, 1, result.stderr);
    return JSON.parse(fs.readFileSync(path.join(out, "comparaison.json"), "utf8"))[0];
  };
  assert.match(run().error, /introuvable/);
  const item = path.join(registry, "components/test");
  fs.mkdirSync(item, { recursive: true });
  fs.writeFileSync(path.join(item, "registry-item.json"), "{");
  assert.match(run().error, /SyntaxError/);
  fs.writeFileSync(path.join(item, "registry-item.json"), JSON.stringify({
    files: [{ type: "hyperframes:composition", path: "composition.html" }],
  }));
  fs.writeFileSync(path.join(pieces, "hf-test.json"), "{");
  assert.match(run().error, /SyntaxError/);
  fs.writeFileSync(path.join(pieces, "hf-test.json"), JSON.stringify({
    composition: { width: 0, height: 100, fps: 30, duration: 120 },
  }));
  assert.match(run().error, /invalides/);
  console.log("Motion web comparison: 4 input failures reported correctly");
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
