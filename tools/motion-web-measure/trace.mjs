// MOTION-WEB measurement (2026-10-11): open each HyperFrames registry
// composition in headless Chromium and read its registered GSAP timelines
// as data instead of pixels, one JSON record per composition. The question
// is how much of a web animation maps onto the CUTMACHINE motion format.
// Usage: node trace.mjs <registry-dir> <cdn-node_modules> <out.jsonl> [filter]
// PLAYWRIGHT_MODULE and CHROMIUM_PATH override the defaults below.
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
import fs from "node:fs";
import path from "node:path";

const [registry, nm, out, filter] = process.argv.slice(2);
const ALIAS = { "three@0.147.0": "three147" };

function localFor(url) {
  const m = url.match(/^https:\/\/cdn\.jsdelivr\.net\/npm\/((?:@[^/]+\/)?[^@/]+)(?:@([^/]+))?\/?(.*)$/);
  if (!m) return null;
  const [, pkg, ver, rest] = m;
  const dir = ALIAS[`${pkg}@${ver}`] || pkg;
  const p = path.join(nm, dir, rest);
  return fs.existsSync(p) && fs.statSync(p).isFile() ? p : null;
}

const pages = [];
for (const kind of ["blocks", "components", "examples"]) {
  for (const name of fs.readdirSync(path.join(registry, kind))) {
    if (filter && !name.includes(filter)) continue;
    const dir = path.join(registry, kind, name);
    const item = path.join(dir, "registry-item.json");
    if (!fs.existsSync(item)) continue;
    const meta = JSON.parse(fs.readFileSync(item, "utf8"));
    const comp = (meta.files || []).find((f) => f.type === "hyperframes:composition" && f.path.endsWith(".html"))
      || (meta.files || []).find((f) => f.path.endsWith(".html"));
    const demo = path.join(dir, "demo.html");
    if (!comp && !fs.existsSync(demo)) continue;
    // Snippets are fragments; their demo.html is the runnable host page.
    const file = fs.existsSync(demo) && (!comp || comp.type !== "hyperframes:composition") ? demo : path.join(dir, comp.path);
    pages.push({ kind, name, file, meta });
  }
}

// Runs inside the page: walks every timeline as data.
const probe = () => {
  const CONTROL = new Set(["duration", "ease", "delay", "stagger", "repeat", "yoyo", "repeatDelay",
    "immediateRender", "overwrite", "paused", "id", "data", "lazy", "callbackScope", "runBackwards",
    "startAt", "inherit", "keyframes", "defaults", "onComplete", "onStart", "onUpdate", "onRepeat",
    "onReverseComplete", "onReverseCompleteParams", "onRepeatParams", "onInterrupt", "onInterruptParams", "onCompleteParams", "onStartParams", "onUpdateParams", "yoyoEase",
    "smoothChildTiming", "autoRemoveChildren", "parent", "reversed", "force3D", "clearProps", "autoRound"]);
  const describeTarget = (t) => {
    if (!(t instanceof Element)) return { kind: typeof t === "object" ? "object" : typeof t };
    const text = t.children.length === 0 ? (t.textContent || "").trim() : "";
    const sib = t.parentElement ? [...t.parentElement.children].filter((c) => c.className === t.className && c.tagName === t.tagName).length : 0;
    return { kind: "element", tag: t.tagName.toLowerCase(), svg: t instanceof SVGElement,
      textLen: text.length, leafText: t.children.length === 0 && text.length > 0, sameClassSiblings: sib };
  };
  const valueKind = (v) => typeof v === "function" ? "function"
    : typeof v === "string" && /^[+\-*]=/.test(v) ? "relative"
    : typeof v === "string" && /random\(/.test(v) ? "random" : typeof v;
  const NAMES = ["none"];
  for (const f of ["power1", "power2", "power3", "power4", "sine", "expo", "circ", "back", "elastic", "bounce"])
    for (const d of ["in", "out", "inOut"]) NAMES.push(`${f}.${d}`);
  const SAMPLES = [0.1, 0.25, 0.5, 0.75, 0.9];
  const sig = (fn) => SAMPLES.map((x) => fn(x));
  const table = NAMES.map((n) => [n, sig(window.gsap.parseEase(n))]);
  const identify = (fn) => {
    const s = sig(fn);
    for (const [n, t] of table) if (t.every((y, k) => Math.abs(y - s[k]) < 1e-9)) return n + "*";
    return null;
  };
  const tweens = [];
  const callbacks = [];
  const walk = (tl, offset, scale, tlName) => {
    for (const c of tl.getChildren(false, true, true)) {
      const start = offset + c.startTime() / scale;
      if (c.getChildren) { walk(c, start, scale * c.timeScale(), tlName); continue; }
      const v = c.vars || {};
      const isCall = c.duration() === 0 && v.onComplete && Object.keys(v).filter((k) => !CONTROL.has(k)).length === 0;
      if (isCall || (c._targets || []).length === 0 && v.onComplete) { callbacks.push({ tl: tlName, start }); continue; }
      const props = {};
      for (const [k, val] of Object.entries(v)) {
        if (CONTROL.has(k)) continue;
        if (k === "css" || k === "attr") { for (const [k2, v2] of Object.entries(val || {})) props[`${k}.${k2}`] = valueKind(v2); continue; }
        props[k] = valueKind(val);
      }
      const targets = c.targets ? c.targets() : [];
      let ease = v.ease === undefined ? "default" : typeof v.ease === "function" ? "custom-fn" : String(v.ease);
      if (typeof c._ease === "function" && (ease === "custom-fn" || ease === "default")) ease = identify(c._ease) || ease;
      tweens.push({
        tl: tlName, start, dur: c.duration() / scale, type: c.constructor?.name, from: !!v.runBackwards,
        props, ease, stagger: v.stagger === undefined ? null : (typeof v.stagger === "object" ? JSON.parse(JSON.stringify(v.stagger, (k, x) => typeof x === "function" ? "fn" : x)) : v.stagger),
        keyframes: v.keyframes !== undefined, repeat: v.repeat || 0, yoyo: !!v.yoyo,
        onUpdate: typeof v.onUpdate === "function", nTargets: targets.length,
        target: targets.length ? describeTarget(targets[0]) : null,
        transformOrigin: targets[0] instanceof Element ? (targets[0].style.transformOrigin || null) : null,
      });
    }
  };
  const tls = window.__timelines || {};
  for (const [name, tl] of Object.entries(tls)) {
    try { walk(tl, 0, tl.timeScale ? tl.timeScale() : 1, name); } catch (e) { callbacks.push({ tl: name, error: String(e) }); }
  }
  const waapi = (document.getAnimations ? document.getAnimations() : []).map((a) => ({
    type: a.constructor.name, props: a.effect && a.effect.getKeyframes ? [...new Set(a.effect.getKeyframes().flatMap((k) => Object.keys(k)))] : [],
  }));
  const dom = {
    canvas: document.querySelectorAll("canvas").length,
    webgl: [...document.querySelectorAll("canvas")].some((c) => { try { return !!(c.getContext("webgl2") || c.getContext("webgl")) && false; } catch { return false; } }),
    svgPaths: document.querySelectorAll("path").length, video: document.querySelectorAll("video").length,
    img: document.querySelectorAll("img").length, elements: document.querySelectorAll("body *").length,
  };
  return { timelines: Object.keys(tls), gsap: !!window.gsap, gsapVersion: window.gsap?.version, tweens, callbacks, waapi, dom };
};

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined,
  args: ["--use-gl=swiftshader", "--enable-unsafe-swiftshader", "--allow-file-access-from-files"] });
const ctx = await browser.newContext({ viewport: { width: 1920, height: 1080 } });
const sink = fs.createWriteStream(out);
let i = 0;
for (const p of pages) {
  i++;
  const page = await ctx.newPage();
  await page.addInitScript(() => { window.__timelines = window.__timelines || {}; });
  const missing = new Set();
  await page.route("**/*", (route) => {
    const u = route.request().url();
    if (u.startsWith("file:") || u.startsWith("data:") || u.startsWith("blob:")) return route.continue();
    const local = localFor(u);
    if (local) return route.fulfill({ path: local });
    missing.add(u.replace(/\?.*$/, ""));
    return route.abort();
  });
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e).slice(0, 200)));
  let rec;
  try {
    await page.goto("file://" + p.file, { waitUntil: "load", timeout: 20000 });
    await page.waitForTimeout(800);
    // Registry compositions ship inside <template>: mount them the way the
    // HyperFrames runtime does, re-creating scripts so they execute in order.
    // Sub-compositions referenced by data-composition-src are fetched by the
    // HyperFrames runtime; read them here and hand their markup to the page.
    const srcs = await page.evaluate(() => [...document.querySelectorAll("[data-composition-src]")]
      .map((e) => e.getAttribute("data-composition-src")));
    const subs = srcs.map((s) => { const f = path.join(path.dirname(p.file), s);
      return fs.existsSync(f) ? fs.readFileSync(f, "utf8") : ""; });
    const mounted = await page.evaluate(async (subs) => {
      const run = async (frag, host) => {
        const scripts = [...frag.querySelectorAll("script")];
        scripts.forEach((s) => s.remove());
        host.appendChild(frag);
        for (const old of scripts) {
          const s = document.createElement("script");
          for (const a of old.attributes) s.setAttribute(a.name, a.value);
          if (old.src) {
            await new Promise((res) => { s.onload = s.onerror = res; document.body.appendChild(s); });
          } else {
            s.textContent = old.textContent;
            document.body.appendChild(s);
          }
        }
      };
      const ensureGsap = async () => {
        if (window.gsap) return;
        await new Promise((res) => { const g = document.createElement("script");
          g.src = "https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"; g.onload = g.onerror = res;
          document.head.appendChild(g); });
      };
      let n = 0;
      const hosts = [...document.querySelectorAll("[data-composition-src]")];
      for (let k = 0; k < hosts.length; k++) {
        if (!subs[k]) continue;
        await ensureGsap();
        const doc = new DOMParser().parseFromString(subs[k], "text/html");
        const tpl = doc.querySelector("template");
        const frag = tpl ? document.importNode(tpl.content, true) : document.createDocumentFragment();
        if (!tpl) for (const c of [...doc.body.childNodes]) frag.appendChild(document.importNode(c, true));
        await run(frag, hosts[k]);
        n++;
      }
      const tls = window.__timelines || {};
      const busy = Object.values(tls).some((t) => t.getChildren && t.getChildren(true, true, true).length);
      if (!n && !busy) {
        for (const tpl of document.querySelectorAll("template")) {
          await ensureGsap();
          await run(tpl.content.cloneNode(true), document.body);
          n++;
        }
      }
      return n;
    }, subs);
    if (mounted) await page.waitForTimeout(800);
    rec = await page.evaluate(probe);
    rec.mountedTemplates = mounted;
  } catch (e) {
    rec = { failed: String(e).slice(0, 300) };
  }
  sink.write(JSON.stringify({ kind: p.kind, name: p.name, tags: p.meta.tags || [], duration: p.meta.duration,
    missing: [...missing], errors, ...rec }) + "\n");
  await page.close();
  if (i % 25 === 0) console.error(`${i}/${pages.length}`);
}
sink.end();
await browser.close();
console.error(`done ${pages.length}`);
