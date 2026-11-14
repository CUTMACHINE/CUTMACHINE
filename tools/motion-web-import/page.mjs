// MOTION-WEB-2026-10 MW4 -- opening a HyperFrames page as its runtime
// would: CDN libraries served from a local node_modules, sub-compositions
// (<template>, data-composition-src) mounted with their scripts run.
import fs from "node:fs";
import path from "node:path";

const ALIAS = { "three@0.147.0": "three147" };

function localFor(url, nm) {
  const m = url.match(/^https:\/\/cdn\.jsdelivr\.net\/npm\/((?:@[^/]+\/)?[^@/]+)(?:@([^/]+))?\/?(.*)$/);
  if (!m) return null;
  const [, pkg, ver, rest] = m;
  const p = path.join(nm, ALIAS[`${pkg}@${ver}`] || pkg, rest);
  return fs.existsSync(p) && fs.statSync(p).isFile() ? p : null;
}

// Routes every request: local files and the CDN copies pass, the rest is
// cut, so a page never reaches the network.
export async function routeLocal(page, nm) {
  await page.addInitScript(() => { window.__timelines = window.__timelines || {}; });
  await page.route("**/*", (route) => {
    const u = route.request().url();
    if (u.startsWith("file:") || u.startsWith("data:") || u.startsWith("blob:")) return route.continue();
    const local = localFor(u, nm);
    return local ? route.fulfill({ path: local }) : route.abort();
  });
}

export async function mount(page, file) {
  await page.goto("file://" + file, { waitUntil: "load", timeout: 20000 });
  await page.waitForTimeout(600);
  const srcs = await page.evaluate(() => [...document.querySelectorAll("[data-composition-src]")]
    .map((e) => e.getAttribute("data-composition-src")));
  const subs = srcs.map((s) => { const f = path.join(path.dirname(file), s);
    return fs.existsSync(f) ? fs.readFileSync(f, "utf8") : ""; });
  await page.evaluate(async (subs) => {
    const run = async (frag, host) => {
      const scripts = [...frag.querySelectorAll("script")];
      scripts.forEach((s) => s.remove());
      host.appendChild(frag);
      for (const old of scripts) {
        const s = document.createElement("script");
        for (const a of old.attributes) s.setAttribute(a.name, a.value);
        if (old.src) await new Promise((res) => { s.onload = s.onerror = res; document.body.appendChild(s); });
        else { s.textContent = old.textContent; document.body.appendChild(s); }
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
      // As the HyperFrames runtime mounts a composition
      // (packages/core/src/runtime/compositionLoader.ts, Apache-2.0): its
      // root is flattened into the host, which takes its size, and its CSS
      // is scoped to the host, so the demo's own #root rules stay out.
      const host = hosts[k];
      const inner = frag.querySelector("[data-composition-id]");
      const authoredId = inner ? inner.getAttribute("data-composition-id") : null;
      if (!host.getAttribute("data-composition-id") && authoredId) host.setAttribute("data-composition-id", authoredId);
      const scope = `[data-composition-id="${host.getAttribute("data-composition-id")}"]`;
      const rootId = inner ? inner.getAttribute("id") : null;
      const box = `${scope} > [data-hf-inner-root]`;
      const scopeSelector = (selector) => {
        let sel = selector.trim();
        if (rootId) sel = sel.replace(new RegExp(`#${rootId}(?![\\w-])`, "g"), `[data-hf-authored-id="${rootId}"]`);
        if (authoredId) {
          const attr = new RegExp(`\\[\\s*data-composition-id\\s*=\\s*["']${authoredId}["']\\s*\\](\\[data-(start|duration)[^\\]]*\\])*`, "g");
          if (attr.test(sel)) return sel.replace(attr, "").trim() === "" ? box : sel.replace(attr, scope);
        }
        if (sel === "*") return sel;
        if (/^(html|body|:root)$/i.test(sel)) return box;
        return `${scope} ${sel}`;
      };
      const scoped = (css) => {
        const sheet = new CSSStyleSheet();
        try { sheet.replaceSync(css); } catch (e) { return css; }
        const write = (rules) => [...rules].map((rule) => {
          if (rule instanceof CSSStyleRule) return `${rule.selectorText.split(",").map(scopeSelector).join(", ")} { ${rule.style.cssText} }`;
          if (rule instanceof CSSMediaRule) return `@media ${rule.conditionText} { ${write(rule.cssRules)} }`;
          return rule.cssText;
        }).join("\n");
        return write(sheet.cssRules);
      };
      const styles = [...doc.head.querySelectorAll("style"), ...frag.querySelectorAll("style")];
      for (const st of styles) {
        const out = document.createElement("style");
        out.textContent = authoredId ? scoped(st.textContent) : st.textContent;
        document.head.appendChild(out);
        st.remove();
      }
      if (inner) {
        for (const attr of ["data-width", "data-height"]) {
          const v = inner.getAttribute(attr);
          if (v) { host.setAttribute(attr, v); host.style[attr === "data-width" ? "width" : "height"] = `${parseFloat(v)}px`; }
        }
        for (const attr of ["data-composition-id", "data-composition-file", "data-start", "data-duration", "data-end",
          "data-track-index", "data-track", "data-composition-src"]) inner.removeAttribute(attr);
        if (rootId) { inner.removeAttribute("id"); inner.setAttribute("data-hf-authored-id", rootId); }
        inner.setAttribute("data-hf-inner-root", "true");
        // The runtime positions its clips: the flattened root measures
        // against its host, not against the demo's stage.
        if (getComputedStyle(host).position === "static") host.style.position = host.classList.contains("clip") ? "absolute" : "relative";
        inner.style.width = host.style.width || "100%";
        inner.style.height = host.style.height || "100%";
      }
      await run(frag, host);
      n++;
    }
    const tls = window.__timelines || {};
    const busy = Object.values(tls).some((t) => t.getChildren && t.getChildren(true, true, true).length);
    if (!n && !busy) for (const tpl of document.querySelectorAll("template")) { await ensureGsap(); await run(tpl.content.cloneNode(true), document.body); }
  }, subs);
  await page.waitForTimeout(600);
  await page.evaluate(() => document.fonts && document.fonts.ready);
}

