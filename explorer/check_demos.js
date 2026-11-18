// Cross-check the explorer's own verification code against the chains the C++ core wrote.
// Runs in Node with a tiny DOM stub: no browser, no network, no npm install — so it belongs in CI.
//   node explorer/check_demos.js
// Exit code 0 = every honest demo verifies and every tampered one is refused.
"use strict";
const fs = require("fs");
const path = require("path");

function makeEl() {
  const t = {
    hidden: false, textContent: "", value: "", className: "", id: "", href: "", title: "", disabled: false,
    children: [], lastChild: null, style: {}, dataset: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    append(...c) { t.children.push(...c); t.lastChild = c[c.length - 1] ?? null; },
    appendChild(c) { t.children.push(c); t.lastChild = c; return c; },
    replaceChildren(...c) { t.children = c; t.lastChild = c[c.length - 1] ?? null; },
    insertBefore(c) { t.children.push(c); return c; },
    removeChild() {}, remove() {}, addEventListener() {}, removeEventListener() {},
    setAttribute() {}, getAttribute() { return null; }, focus() {}, click() {},
    querySelector() { return makeEl(); }, querySelectorAll() { return []; },
    getBoundingClientRect() { return { top: 0, left: 0, width: 0, height: 0 }; },
  };
  return t;
}
const els = new Map();
const byId = (id) => { if (!els.has(id)) els.set(id, makeEl()); return els.get(id); };

global.document = {
  getElementById: byId,
  querySelector: (s) => byId(String(s).replace(/^#/, "")),
  querySelectorAll: () => [],
  createElement: () => makeEl(),
  createTextNode: (x) => ({ textContent: String(x) }),
  addEventListener() {},
  documentElement: makeEl(),
  body: makeEl(),
};
global.window = { addEventListener() {}, matchMedia: () => ({ matches: false, addEventListener() {} }), location: { href: "file:///" } };
global.localStorage = { getItem: () => null, setItem() {}, removeItem() {} };

const src = fs.readFileSync(path.join(__dirname, "script.src.js"), "utf8");
const api = new Function("document", "window", "localStorage", "navigator", src + "\n;return { parseChain, verifyChain };")(
  global.document, global.window, global.localStorage, global.navigator);

// v8.10: any directories given on the command line are checked too — CI passes the chains that
// taz_gen_veto_demo wrote, so the veto rules are tested on chains the C++ core produced.
const extra = [];
for (const dir of process.argv.slice(2)) {
  let entries = [];
  try { entries = fs.readdirSync(dir); } catch { console.log("  skip  " + dir + " (not present)"); continue; }
  for (const e of entries.filter((x) => x.endsWith(".json")).sort()) extra.push(path.join(dir, e));
}
const demos = ["demo_chain.json", "demo_safety_chain.json", "demo_recovery_chain.json"].map((f) => path.join(__dirname, f)).concat(extra);
let failures = 0;
const report = (ok, label, detail) => {
  console.log((ok ? "  ok   " : "  FAIL ") + label + (detail ? " — " + detail : ""));
  if (!ok) failures++;
};

(async () => {
  for (const name of demos) {
    const text = fs.readFileSync(name, "utf8");
    let r;
    try { r = await api.verifyChain(api.parseChain(text)); }
    catch (e) { report(false, path.basename(name), "threw: " + (e && e.message ? e.message : e)); continue; }
    const checks = Object.values(r.cats).reduce((n, c) => n + c.n, 0);
    const passed = Object.values(r.cats).reduce((n, c) => n + c.ok, 0);
    report(r.fails.length === 0 && !r.stateBroken, path.basename(name),
      r.fails.length === 0 ? passed + "/" + checks + " checks" : r.fails[0]);
  }

  // A tampered copy must be refused — otherwise the page would bless anything.
  const base = JSON.parse(fs.readFileSync(path.join(__dirname, "demo_chain.json"), "utf8"));
  const tx = base.blocks.flatMap((b) => b.transactions)[0];
  tx.amount = (BigInt(tx.amount) + 1n).toString();
  let r;
  try { r = await api.verifyChain(api.parseChain(JSON.stringify(base))); }
  catch (e) { report(true, "tampered demo is refused (at parse time)", e && e.message); r = null; }
  if (r) report(r.fails.length > 0 || r.stateBroken, "tampered demo is refused", r.fails[0] || "no failure recorded");

  console.log(failures === 0 ? "explorer cross-check: all demos agree with the core" : "explorer cross-check: " + failures + " problem(s)");
  process.exit(failures === 0 ? 0 : 1);
})();
