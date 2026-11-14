// FI3/FI5 -- ROADMAP.md. Local GSAP scenes against the real engine, plus
// technical failures in the comparison pipeline. No registry or network.
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync, spawnSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const [engine, ffmpeg] = process.argv.slice(2);
const modules = path.resolve(process.env.MOTION_WEB_NODE_MODULES || "node_modules");
const gsapFile = path.join(modules, "gsap/dist/gsap.min.js");
const playwrightFile = path.join(modules, "playwright/index.mjs");
const playwright = process.env.PLAYWRIGHT_MODULE ||
  (fs.existsSync(playwrightFile) ? pathToFileURL(playwrightFile).href : "playwright");
let chromium;
try {
  ({ chromium } = await import(playwright));
} catch (e) {
  if (e.code !== "ERR_MODULE_NOT_FOUND") throw e;
  console.log("SKIP: installer Playwright et GSAP puis définir MOTION_WEB_NODE_MODULES (tools/motion-web-import/README.md)");
  process.exit(77);
}
if (!fs.existsSync(gsapFile) || !fs.existsSync(process.env.CHROMIUM_PATH || chromium.executablePath())) {
  console.log("SKIP: GSAP ou Chromium absent ; voir tools/motion-web-import/README.md");
  process.exit(77);
}

const work = fs.mkdtempSync(path.join(os.tmpdir(), "cutmachine-web-test-"));
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const html = '<style>*{box-sizing:border-box}body{margin:0}#root{position:relative;width:400px;height:100px}#dot{position:absolute;left:0;top:0;width:20px;height:20px;background:red}</style>' +
  '<div id="root" data-composition-id="test" data-width="400" data-height="100" data-fps="30" data-duration="4"><div id="dot"></div></div>';
const env = { ...process.env, CUTMACHINE_LOG: "0", CUTMACHINE_PROJECT_ARCHIVE: "off" };
try {
  const frames = [0, 8, 15, 18, 23, 30, 45, 53, 60, 75, 90, 119];
  for (const kind of ["repeat-yoyo", "repeat-delay", "child-timescale", "nested-timescale",
    "nested-repeat", "root-timescale", "root-repeat", "infinite-repeat"]) {
    const page = await browser.newPage();
    await page.route("http*://**/*", (route) => route.abort());
    await page.setContent(html);
    await page.addScriptTag({ path: gsapFile });
    await page.evaluate((kind) => {
      const tl = gsap.timeline({ paused: true });
      let owner = tl;
      if (kind.startsWith("nested-")) {
        owner = gsap.timeline(kind === "nested-repeat" ? { repeat: 1, yoyo: true, repeatDelay: 0.25 } : {});
      }
      owner.to("#dot", { x: 120, duration: 1, ease: "none",
        ...(kind === "repeat-yoyo" ? { repeat: 2, yoyo: true } : {}),
        ...(kind === "repeat-delay" ? { repeat: 2, yoyo: true, repeatDelay: 0.25 } : {}),
        ...(kind === "infinite-repeat" ? { repeat: -1, yoyo: true } : {}),
      });
      if (kind === "child-timescale") owner.getChildren()[0].timeScale(2);
      if (kind === "nested-timescale") {
        owner.getChildren()[0].timeScale(0.5);
        owner.timeScale(2);
      }
      if (owner !== tl) tl.add(owner, 0.25);
      if (kind === "root-timescale") tl.timeScale(2);
      if (kind === "root-repeat") tl.repeat(2).yoyo(true);
      window.__timelines = { test: tl };
    }, kind);
    await page.addScriptTag({ path: path.join(root, "tools/motion-web-import/extract.js") });
    const result = await page.evaluate(() => window.__cmExtract({}));
    assert.equal(result.error, undefined, kind);
    const file = path.join(work, `${kind}.json`);
    fs.writeFileSync(file, JSON.stringify(result.composition));
    for (const frame of frames) {
      const expected = await page.evaluate((t) => {
        window.__timelines.test.seek(0, false);
        window.__timelines.test.seek(t, false);
        return gsap.getProperty("#dot", "x");
      }, frame / 30);
      const evaluated = JSON.parse(execFileSync(engine, ["--describe-motion-frame", file, String(frame)],
        { encoding: "utf8", env, timeout: 15000 }));
      const node = evaluated.root.children.find((n) => n.id === "dot");
      const actual = node.transform?.translateX || 0;
      assert.ok(Math.abs(actual - expected) <= 0.002, `${kind} frame ${frame}: engine ${actual}, Chrome ${expected}`);
    }
    await page.close();
    console.log(`${kind}: ${frames.length} frames match Chrome`);
  }

  // A complete local comparison succeeds; renderer and FFmpeg failures
  // must produce a nonzero status and a readable report.
  const item = path.join(work, "registry/components/test");
  const pieces = path.join(work, "pieces");
  fs.mkdirSync(item, { recursive: true });
  fs.mkdirSync(pieces);
  fs.writeFileSync(path.join(item, "registry-item.json"), JSON.stringify({
    files: [{ type: "hyperframes:composition", path: "composition.html" }],
  }));
  fs.writeFileSync(path.join(item, "composition.html"), html +
    '<script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>' +
    '<script>window.__timelines={test:gsap.timeline({paused:true}).to("#dot",{x:120,duration:1,ease:"none"})};</script>');
  fs.writeFileSync(path.join(pieces, "hf-test.json"), JSON.stringify({ composition: {
    width: 400, height: 100, fps: 30, duration: 120, root: { type: "fill", children: [{
      type: "box", style: { position: "absolute", width: 20, height: 20, backgroundColor: "#ff0000" },
      animate: { translateX: { interpolate: { frames: [0, 30], values: [0, 120] } } },
    }] },
  } }));
  const silentFfmpeg = path.join(work, "silent-ffmpeg");
  fs.writeFileSync(silentFfmpeg, "#!/bin/sh\nexit 0\n", { mode: 0o755 });
  for (const failure of ["none", "rerun", "engine", "ffmpeg", "empty-log", "empty"]) {
    if (failure === "empty") {
      fs.appendFileSync(path.join(item, "composition.html"), "<style>#dot{opacity:0}</style>");
      const file = path.join(pieces, "hf-test.json");
      const piece = JSON.parse(fs.readFileSync(file, "utf8"));
      piece.composition.root.style = { opacity: 0 };
      fs.writeFileSync(file, JSON.stringify(piece));
    }
    const successful = failure === "none" || failure === "rerun" || failure === "empty";
    const out = path.join(work, `compare-${failure === "rerun" ? "none" : failure}`);
    const result = spawnSync(process.execPath, [path.join(root, "tools/motion-web-import/compare.mjs"),
      path.join(work, "registry"), modules, pieces, out, "test"], { encoding: "utf8", timeout: 60000,
      env: { ...env, PLAYWRIGHT_MODULE: playwright, CUTMACHINE: failure === "engine" ? path.join(work, "missing-engine") : engine,
        FFMPEG: failure === "empty-log" ? silentFfmpeg : failure === "ffmpeg" ? path.join(work, "missing-ffmpeg") : ffmpeg } });
    const report = JSON.parse(fs.readFileSync(path.join(out, "comparaison.json"), "utf8"))[0];
    assert.equal(result.status, successful ? 0 : 1, `${result.stderr}\n${JSON.stringify(report)}`);
    if (successful) {
      assert.equal(report.frames.length, 6);
      assert.ok(report.frames.every((f) => !f.error &&
        (failure === "empty" ? f.empty && f.ssim === null && f.psnr === null : f.ssim > 0.99)), JSON.stringify(report));
    }
    if (failure === "engine") assert.ok(report.frames.every((f) => f.error));
    if (failure === "ffmpeg" || failure === "empty-log") assert.match(report.error, /FFmpeg/);
    console.log(`comparison ${failure}: exit status and report agree`);
  }
} finally {
  await browser.close();
  fs.rmSync(work, { recursive: true, force: true });
}
