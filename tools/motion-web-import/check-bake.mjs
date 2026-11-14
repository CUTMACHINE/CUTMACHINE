// MOTION-WEB-2026-10 MW4 -- checks bakePath (extract.js) against Chromium:
// each path drawn under an SVG transform, and drawn baked, must cover the
// same pixels (fill only: a stroke keeps its width when baked, by design).
// Usage: node check-bake.mjs extract.js
// PLAYWRIGHT_MODULE and CHROMIUM_PATH as elsewhere.
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
import fs from "node:fs";
const src = fs.readFileSync(process.argv[2], "utf8");
const start = src.indexOf("    function bakePath(");
const end = src.indexOf("    // The transform attributes between");
const fn = src.slice(start, end);
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage();
await page.setContent("<body></body>");
const res = await page.evaluate((fn) => {
  const round = (v, digits = 3) => { const k = 10 ** digits; const r = Math.round(v * k) / k; return Object.is(r, -0) ? 0 : r; };
  const bakePath = eval("(" + fn.trim().replace(/^function bakePath/, "function") + ")");
  const cases = [
    ["M10 10 L60 10 L60 40 Z", "translate(20 15)"],
    ["M10 10 h40 v20 h-40 z", "rotate(30 50 50)"],
    ["M20 50 A30 20 0 1 0 80 50 A30 20 0 1 0 20 50Z", "matrix(1.2 0.3 -0.2 0.9 5 7)"],
    ["M10 80 C 40 10, 65 10, 95 80 S 150 150, 180 80", "scale(0.5 1.5)"],
    ["M10 80 Q 52.5 10, 95 80 T 180 80", "translate(5 -20) rotate(-12)"],
    ["m10 10 l20 0 a10 10 0 0 1 10 10 l0 20 a10 10 0 0 1 -10 10z", "skewX(20)"],
  ];
  const diff = [];
  for (const [d, t] of cases) {
    const draw = (inner) => {
      const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">${inner}</svg>`;
      return svg;
    };
    const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
    g.setAttribute("transform", t);
    const m = g.transform.baseVal.consolidate().matrix;
    const baked = bakePath(d, m);
    const raster = (svg) => new Promise((ok) => { const img = new Image(); img.onload = () => { const c = document.createElement("canvas"); c.width = c.height = 200; const x = c.getContext("2d"); x.drawImage(img, 0, 0); ok(x.getImageData(0, 0, 200, 200).data); }; img.src = "data:image/svg+xml," + encodeURIComponent(svg); });
    diff.push(Promise.all([raster(draw(`<g transform="${t}"><path d="${d}" fill="black"/></g>`)), raster(draw(`<path d="${baked}" fill="black"/>`))]).then(([a, b]) => {
      let n = 0, painted = 0; for (let i = 3; i < a.length; i += 4) { if (a[i] > 0) painted++; if (Math.abs(a[i] - b[i]) > 64 || Math.abs(a[i-3]-b[i-3]) > 64) n++; } return `${t}: ${n} px off of ${painted}`;
    }));
  }
  return Promise.all(diff);
}, fn);
console.log(res.join("\n"));
await browser.close();
// Antialiasing along the arcs, approximated by cubics, allows a few pixels.
process.exit(res.every((line) => Number(line.match(/: (\d+) px off/)[1]) <= 12) ? 0 : 1);
