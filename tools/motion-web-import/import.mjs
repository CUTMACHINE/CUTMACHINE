// MOTION-WEB-2026-10 MW4 -- imports HyperFrames registry compositions as
// motion pieces (motion-piece.v1). Each page is opened in Chromium,
// extract.js reads it as data, and the piece is checked by the engine
// (VALIDATOR: a binary that runs ParseMotionPiece and evaluates every
// frame). A value the format refuses is taken out and said in the piece's
// report, never forced through.
//
// Usage: node import.mjs <registry-dir> <cdn-node_modules> <out-dir> [names...]
// PLAYWRIGHT_MODULE, CHROMIUM_PATH and VALIDATOR as in tools/motion-web-measure.
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { mount, routeLocal } from "./page.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const [registry, nm, out, ...only] = process.argv.slice(2);
const ORIGIN_COMMIT = process.env.HYPERFRAMES_COMMIT || "8bca7c4";
fs.mkdirSync(out, { recursive: true });
const pages = [];
for (const kind of ["blocks", "components", "examples"]) {
  for (const name of fs.readdirSync(path.join(registry, kind))) {
    if (only.length && !only.includes(name)) continue;
    const dir = path.join(registry, kind, name);
    const item = path.join(dir, "registry-item.json");
    if (!fs.existsSync(item)) continue;
    const meta = JSON.parse(fs.readFileSync(item, "utf8"));
    const comp = (meta.files || []).find((f) => f.type === "hyperframes:composition" && f.path.endsWith(".html"));
    const demo = path.join(dir, "demo.html");
    const file = comp ? path.join(dir, comp.path) : fs.existsSync(demo) ? demo
      : (meta.files || []).find((f) => f.path.endsWith(".html")) ? path.join(dir, meta.files.find((f) => f.path.endsWith(".html")).path) : null;
    if (file) pages.push({ kind, name, file, meta });
  }
}

const extractSource = fs.readFileSync(path.join(here, "extract.js"), "utf8");
// A browser that a page left stuck (SwiftShader busy with a render loop)
// is replaced; so is every browser after 20 pages, before slow pages pile up.
const launch = async () => {
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined,
    args: ["--use-gl=swiftshader", "--enable-unsafe-swiftshader", "--allow-file-access-from-files"] });
  const ctx = await browser.newContext({ viewport: { width: 1920, height: 1080 } });
  return { browser, ctx };
};
const within = (ms, what, promise) => {
  let timer;
  const limit = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(what)), ms); });
  return Promise.race([limit, promise]).finally(() => clearTimeout(timer));
};
let { browser, ctx } = await launch();
const relaunch = async () => {
  await within(15000, "close", browser.close()).catch(() => {});
  ({ browser, ctx } = await launch());
};

function formatName(w, h) {
  const g = (a, b) => (b ? g(b, a % b) : a);
  const d = g(w, h);
  return `${w / d}x${h / d}`;
}

// Deletes the value at a parser path ("root.children[0].style.filter").
function removeAt(composition, errorPath) {
  const steps = errorPath.replace(/^composition\./, "").split(".").flatMap((s) => {
    const m = s.match(/^([^\[]+)((\[\d+\])*)$/);
    if (!m) return [s];
    return [m[1], ...[...m[2].matchAll(/\[(\d+)\]/g)].map((x) => Number(x[1]))];
  });
  // Walk to the deepest style/animate entry or node the path names.
  let cursor = composition;
  const trail = [];
  for (const step of steps) {
    if (cursor === undefined || cursor === null) break;
    trail.push([cursor, step]);
    cursor = cursor[step];
  }
  for (let i = trail.length - 1; i >= 0; i--) {
    const [holder, key] = trail[i];
    const parentKey = i > 0 ? trail[i - 1][1] : null;
    if (parentKey === "style" || parentKey === "animate" || key === "backgroundImage") {
      const value = JSON.stringify(holder[key]).slice(0, 160);
      delete holder[key];
      return `${errorPath} = ${value}`;
    }
    if (parentKey === "children" && Array.isArray(holder)) {
      holder.splice(key, 1);
      return `${errorPath} (nœud retiré)`;
    }
  }
  return null;
}

const validator = process.env.VALIDATOR;
const report = [];
let index = 0;
async function readPage(p) {
  let page = null;
  const name = `hf-${p.name}`.slice(0, 64);
  const entry = { name, kind: p.kind, source: p.name };
  try {
    // A page that never settles (a render loop under SwiftShader) costs
    // itself, not the whole import: two minutes, then it is reported.
    const result = await within(120000, "page trop longue à lire (120 s)", (async () => {
      page = await ctx.newPage();
      await routeLocal(page, nm);
      await mount(page, p.file);
      await page.addScriptTag({ content: extractSource });
      return page.evaluate(() => window.__cmExtract({}));
    })());
    if (result.error) throw new Error(result.error);
    const { composition, notes, stats } = result;
    composition.formats = { [formatName(composition.width, composition.height)]: { width: composition.width, height: composition.height } };
    const tags = (p.meta.tags || []).join(", ");
    const props = Object.keys(composition.props || {});
    const piece = {
      format: "motion-piece.v1",
      description: `${p.meta.title || p.name}: ${p.meta.description || ""} (${composition.duration} frames at ${composition.fps} fps)` +
        (props.length ? ` Props: ${props.join(", ")}.` : "") + (tags ? ` Tags: ${tags}.` : ""),
      origin: `HyperFrames registry ${p.kind}/${p.name} (heygen-com/hyperframes ${ORIGIN_COMMIT}, Apache-2.0), imported by tools/motion-web-import (MOTION-WEB-2026-10 MW4).`,
      composition,
    };
    const file = path.join(out, `${name}.json`);
    const refused = [];
    let verdict = null;
    for (let attempt = 0; attempt < 40; attempt++) {
      fs.writeFileSync(file, JSON.stringify(piece, null, 2) + "\n");
      if (!validator) break;
      verdict = JSON.parse(execFileSync(validator, [file], { encoding: "utf8", timeout: 120000 }).trim());
      if (verdict.ok) break;
      const where = verdict.path || ((verdict.message || "").match(/ at ([\w.\[\]]+): /) || [])[1];
      const at = where ? removeAt(composition, where) : null;
      if (!at) break;
      refused.push(`${at} : ${verdict.message}`);
    }
    entry.ok = validator ? !!(verdict && verdict.ok) : null;
    if (verdict && !verdict.ok) entry.error = verdict;
    entry.stats = stats;
    entry.notes = notes;
    entry.refused = refused;
    if (!entry.ok && validator) fs.renameSync(file, file + ".refused");
  } catch (e) {
    entry.ok = false;
    entry.error = String(e).slice(0, 300);
  }
  const closed = page ? await within(15000, "close", page.close()).then(() => true, () => false) : true;
  if (!closed || /trop longue/.test(entry.error || "")) await relaunch();
  return entry;
}
for (const p of pages) {
  index++;
  if (index % 20 === 1 && index > 1) await relaunch();
  let entry = await readPage(p);
  // A WebGL page that did not load in time under a busy machine gets a
  // second chance, in a fresh browser.
  if (/page\.goto: Timeout/.test(entry.error || "")) {
    await relaunch();
    entry = await readPage(p);
  }
  report.push(entry);
  console.error(`${p.name}: ${entry.ok ? "ok" : "refusée"}`);
  if (index % 20 === 0) console.error(`${index}/${pages.length}`);
}
await within(15000, "close", browser.close()).catch(() => {});
fs.writeFileSync(path.join(out, "import-report.json"), JSON.stringify(report, null, 1) + "\n");
const ok = report.filter((r) => r.ok).length;
console.error(`${ok}/${report.length} pieces accepted by the engine`);
process.exit(0);
