// MOTION-WEB-2026-10 MW4 -- an imported piece against the page it came
// from, on the Mac: Chromium renders the HyperFrames page at control
// frames, CUTMACHINE renders the piece at the same frames
// (--render-motion-frame), FFmpeg measures the gap over a grey ground (SSIM
// and PSNR) and lays the two side by side for a person to look at. Both
// sides must see the same fonts: install the faces the pieces name, or pass
// FONT_DIR to both (Chromium reads installed fonts only).
//
// Usage: node compare.mjs <registry-dir> <cdn-node_modules> <pieces-dir> <out-dir> [names...]
// CUTMACHINE: the engine binary (default ./build/cutmachine); FFMPEG
// (default ffmpeg); FONT_DIR, PLAYWRIGHT_MODULE, CHROMIUM_PATH as elsewhere.
import fs from "node:fs";
import path from "node:path";
import { execFileSync, spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mount, routeLocal } from "./page.mjs";

const [registry, nm, piecesDir, out, ...only] = process.argv.slice(2);
const engine = process.env.CUTMACHINE || "./build/cutmachine";
const ffmpeg = process.env.FFMPEG || "ffmpeg";
const fontDir = process.env.FONT_DIR;
// A control set covering the shapes of the library: lower thirds, titles,
// captions, cards, transitions and the generic entrance.
const CONTROL = ["lt-soft-pill", "lt-kicker-name", "lt-mask-reveal", "lt-stack-bars",
  "titlecard-calm", "titlecard-lockup", "headline-slam", "cta-lockup", "blur-in",
  "stagger-cascade", "caption-highlight", "caption-pill-karaoke", "testimonial-card",
  "notification-stack", "push-in", "logo-sting", "marker-highlight", "kinetic-type"];
const names = only.length ? only : CONTROL;
fs.mkdirSync(out, { recursive: true });

// FI5 -- ROADMAP.md. A failed process is not an empty picture or a missing
// score: keep the technical failure in the report and in the exit status.
function checkedFfmpeg(args) {
  const run = spawnSync(ffmpeg, args, { encoding: "utf8", timeout: 120000 });
  if (run.error || run.status !== 0) {
    throw new Error(`FFmpeg : ${run.error?.message || run.stderr || `code ${run.status}, signal ${run.signal}`}`.slice(0, 300));
  }
  return run;
}

function pageFile(name) {
  for (const kind of ["blocks", "components", "examples"]) {
    const dir = path.join(registry, kind, name);
    const item = path.join(dir, "registry-item.json");
    if (!fs.existsSync(item)) continue;
    const meta = JSON.parse(fs.readFileSync(item, "utf8"));
    const comp = (meta.files || []).find((f) => f.type === "hyperframes:composition" && f.path.endsWith(".html"));
    if (comp) return path.join(dir, comp.path);
    if (fs.existsSync(path.join(dir, "demo.html"))) return path.join(dir, "demo.html");
  }
  return null;
}

// The box of what a picture paints (alpha above 8/255), from FFmpeg's bbox.
function paintedBox(file) {
  const run = checkedFfmpeg(["-hide_banner", "-i", file, "-vf", "alphaextract,bbox=min_val=8", "-f", "null", "-"]);
  // bbox reports an empty frame as just n/pts/pts_time, without coordinates.
  // The frame line must still exist: no output is a failed measurement.
  const line = [...(run.stderr || "").matchAll(/\[Parsed_bbox_[^\]]+\]\s+n:\d+[^\r\n]*/g)].pop();
  if (!line) throw new Error("FFmpeg n'a pas rendu la boîte alpha de l'image");
  const m = line[0].match(/x1:(-?\d+) x2:(-?\d+) y1:(-?\d+) y2:(-?\d+)/);
  return m && +m[2] >= +m[1] && +m[4] >= +m[3]
    ? { x1: +m[1], x2: +m[2], y1: +m[3], y2: +m[4] } : null;
}

function scores(a, b, w, h) {
  // Measured over what either side paints: over the whole frame, a small
  // lower third against an empty picture already scores 0.99.
  const boxes = [paintedBox(a), paintedBox(b)].filter(Boolean);
  if (!boxes.length) return { log: "", empty: true };
  const x1 = Math.max(0, Math.min(...boxes.map((r) => r.x1)) - 8);
  const y1 = Math.max(0, Math.min(...boxes.map((r) => r.y1)) - 8);
  const x2 = Math.min(w - 1, Math.max(...boxes.map((r) => r.x2)) + 8);
  const y2 = Math.min(h - 1, Math.max(...boxes.map((r) => r.y2)) + 8);
  const cw = Math.max(2, (x2 - x1 + 1) & ~1);
  const ch = Math.max(2, (y2 - y1 + 1) & ~1);
  const crop = `crop=${cw}:${ch}:${x1}:${y1}`;
  // FFmpeg prints its measures on stderr.
  const run = checkedFfmpeg(["-hide_banner", "-i", a, "-i", b, "-filter_complex",
    `color=gray:s=${w}x${h}[g0];color=gray:s=${w}x${h}[g1];` +
    `[g0][0:v]overlay=format=auto:shortest=1,${crop}[a];[g1][1:v]overlay=format=auto:shortest=1,${crop}[b];` +
    `[a]split[a1][a2];[b]split[b1][b2];[a1][b1]ssim;[a2][b2]psnr`, "-f", "null", "-"]);
  return { log: run.stderr || "", box: { x: x1, y: y1, width: cw, height: ch } };
}

let browser = null;
const rows = [];
for (const name of names) {
  const pieceFile = path.join(piecesDir, `hf-${name}.json`);
  let file;
  let piece;
  try {
    file = pageFile(name);
    if (fs.existsSync(pieceFile)) piece = JSON.parse(fs.readFileSync(pieceFile, "utf8"));
  } catch (e) {
    rows.push({ name, error: String(e).slice(0, 300) });
    continue;
  }
  if (!fs.existsSync(pieceFile) || !file) { rows.push({ name, error: "pièce ou page introuvable" }); continue; }
  if (!piece?.composition || ![piece.composition.width, piece.composition.height, piece.composition.duration, piece.composition.fps].every((v) => Number.isFinite(v) && v > 0)) {
    rows.push({ name, error: "dimensions, durée ou cadence de la pièce invalides" });
    continue;
  }
  const { width: w, height: h, duration, fps } = piece.composition;
  const composition = path.join(out, `${name}.composition.json`);
  fs.writeFileSync(composition, JSON.stringify(piece.composition));
  const frames = [...new Set([0.08, 0.2, 0.35, 0.5, 0.75, 0.95].map((k) => Math.min(duration - 1, Math.round(duration * k))))];
  if (!browser) {
    const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined,
      args: ["--allow-file-access-from-files", "--force-color-profile=srgb"] });
  }
  // The page keeps its own stage size (a demo lays out on 1920x1080); only
  // the composition's box is cut out of it.
  const context = await browser.newContext({ viewport: { width: Math.max(1920, w), height: Math.max(1080, h) }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  await routeLocal(page, nm);
  const row = { name, frames: [] };
  try {
    await mount(page, file);
    for (const frame of frames) {
      const chrome = path.join(out, `${name}-${frame}-chrome.png`);
      const engineOut = path.join(out, `${name}-${frame}-cutmachine.png`);
      // The HyperFrames runtime hides a clip outside its window; so does
      // this, then every timeline is seeked to the frame.
      const box = await page.evaluate((t) => {
        for (const el of document.querySelectorAll("[data-start]")) {
          if (el.hasAttribute("data-composition-id")) continue;
          const start = Number(el.getAttribute("data-start"));
          const length = Number(el.getAttribute("data-duration"));
          if (!Number.isFinite(start)) continue;
          const shown = t >= start - 1e-9 && (!Number.isFinite(length) || t < start + length - 1e-9);
          el.style.visibility = shown ? "" : "hidden";
        }
        // From 0 forward, as extract.js reads: sets on the way are rendered.
        for (const tl of Object.values(window.__timelines || {})) if (tl.seek) { tl.seek(0, false); tl.seek(t, false); }
        // The root extract.js reads: the composition whose timeline holds
        // the most tweens, not the demo's own scene.
        let best = null;
        let most = -1;
        for (const [id, tl] of Object.entries(window.__timelines || {})) {
          const n = tl.getChildren ? tl.getChildren(true, true, true).length : 0;
          if (n > most) { most = n; best = id; }
        }
        const root = document.querySelector(`[data-composition-id="${best}"]`) || document.querySelector("[data-composition-id]");
        // MW4 -- only the piece is measured: the page around it (its
        // stage, the elements beside it) is hidden, and a mounted
        // component's host loses its demo card (background, shadow,
        // border, corners), which extract.js leaves out of the piece too.
        // Borders turn transparent rather than go, so nothing moves.
        for (let e = root; e.parentElement; e = e.parentElement) {
          for (const sibling of e.parentElement.children) if (sibling !== e) sibling.style.visibility = "hidden";
          e.parentElement.style.background = "transparent";
          e.parentElement.style.boxShadow = "none";
        }
        if (root.hasAttribute("data-composition-src") || root.querySelector(":scope > [data-hf-inner-root]")) {
          Object.assign(root.style, { background: "transparent", boxShadow: "none", borderColor: "transparent",
            outlineColor: "transparent", borderRadius: "0", filter: "none" });
        }
        const r = root.getBoundingClientRect();
        return { x: r.x, y: r.y };
      }, frame / fps);
      await page.screenshot({ path: chrome, omitBackground: true, clip: { x: box.x, y: box.y, width: w, height: h } });
      // The engine refuses overwrites. Publish a new image only after it
      // rendered, so a comparison can be rerun in the same output folder.
      const rendered = path.join(out, `.${name}-${frame}-${randomUUID()}.png`);
      const args = ["--render-motion-frame", composition, String(frame), rendered];
      if (fontDir) args.push("--font-dir", fontDir);
      let engineError = null;
      try {
        execFileSync(engine, args, { encoding: "utf8", timeout: 120000 });
        fs.renameSync(rendered, engineOut);
      } catch (e) {
        engineError = String(e.stdout || e.message).slice(0, 300);
      } finally {
        fs.rmSync(rendered, { force: true });
      }
      const entry = { frame };
      if (engineError) entry.error = engineError;
      else {
        const measured = scores(chrome, engineOut, w, h);
        const log = measured.log;
        entry.box = measured.box;
        if (measured.empty) entry.empty = true;
        const ssim = log.match(/SSIM .*All:([\d.]+)/);
        const psnr = log.match(/PSNR .*average:([\d.inf]+)/);
        entry.ssim = ssim ? Number(ssim[1]) : null;
        entry.psnr = psnr ? (psnr[1] === "inf" ? "inf" : Number(psnr[1])) : null;
        if (!entry.empty && (entry.ssim === null || entry.psnr === null)) throw new Error("FFmpeg n'a pas rendu les mesures SSIM / PSNR");
        // Side by side over grey, Chrome left, CUTMACHINE right.
        checkedFfmpeg(["-hide_banner", "-loglevel", "error", "-y", "-i", chrome, "-i", engineOut, "-filter_complex",
            `color=gray:s=${w}x${h}[g0];color=gray:s=${w}x${h}[g1];[g0][0:v]overlay=shortest=1[a];[g1][1:v]overlay=shortest=1[b];[a][b]hstack,scale=1280:-2`,
            "-frames:v", "1", path.join(out, `${name}-${frame}-cote-a-cote.png`)]);
      }
      row.frames.push(entry);
    }
  } catch (e) {
    row.error = String(e).slice(0, 300);
  }
  await context.close();
  const measured = row.frames.filter((f) => f.ssim !== undefined && f.ssim !== null);
  row.ssim_min = measured.length ? Math.min(...measured.map((f) => f.ssim)) : null;
  rows.push(row);
  console.error(`${name}: ${row.error || row.frames.map((f) => f.error ? "erreur" : f.ssim?.toFixed(3)).join(" ")}`);
}
await browser?.close();
fs.writeFileSync(path.join(out, "comparaison.json"), JSON.stringify(rows, null, 1) + "\n");
const lines = rows.map((r) => `| ${r.name} | ${r.error ? r.error.replace(/\|/g, "/") :
  r.frames.map((f) => `${f.frame}: ${f.error ? "erreur moteur" : f.empty ? "images vides" : `${f.ssim?.toFixed(3)} / ${f.psnr === "inf" ? "∞" : Number.isFinite(f.psnr) ? f.psnr.toFixed(1) : "indisponible"} dB`}`).join(" ; ")} |`);
fs.writeFileSync(path.join(out, "comparaison.md"),
  "# Pièces importées contre Chrome\n\nSSIM / PSNR sur fond gris, mesurés dans le cadre de ce que l'un ou l'autre dessine, image par image ; `*-cote-a-cote.png` : Chrome à gauche, CUTMACHINE à droite.\n\n" +
  "| Pièce | Images de contrôle |\n|---|---|\n" + lines.join("\n") + "\n");
process.exitCode = rows.some((r) => r.error || r.frames?.some((f) => f.error)) ? 1 : 0;
