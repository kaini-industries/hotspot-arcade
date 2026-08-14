import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

function makeClassList() {
  const set = new Set(["hide"]);
  return {
    add(x) { set.add(x); }, remove(x) { set.delete(x); },
    toggle(x, on) { on ? set.add(x) : set.delete(x); },
    contains(x) { return set.has(x); },
  };
}
function element() {
  const listeners = new Map();
  const el = {
    textContent: "", className: "", innerHTML: "", style: {}, value: "",
    children: [], disabled: false, classList: makeClassList(),
    firstElementChild: { style: {}, classList: makeClassList() },
    appendChild(child) { this.children.push(child); return child; },
    addEventListener(type, fn) { listeners.set(type, fn); },
    setAttribute() {}, focus() {},
  };
  Object.defineProperty(el, "childElementCount", { get() { return el.children.length; } });
  return el;
}

const elements = new Map();
const document = {
  hidden: false,
  body: element(),
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  },
  createElement() { return element(); },
  createTextNode(text) { return { textContent: text }; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
let mono = 1000;
let wall = 10_000;
const intervals = [];
const context = {
  __HA_TEST__: true,
  document,
  navigator: {},
  location: { protocol: "http:", host: "192.168.4.1", hostname: "192.168.4.1", search: "" },
  URLSearchParams,
  Uint8Array,
  console,
  performance: { now() { return mono; } },
  Date: { now() { return wall; } },
  crypto: { getRandomValues(bytes) { bytes.fill(2); return bytes; } },
  localStorage: { getItem() { return null; }, setItem() {} },
  isFinite,
  setInterval(fn, ms) {
    const timer = { fn, ms, active: true };
    intervals.push(timer);
    return timer;
  },
  clearInterval(timer) { if (timer) timer.active = false; },
  setTimeout() { return { active: true }; }, clearTimeout(timer) { if (timer) timer.active = false; },
  requestAnimationFrame() { return 1; }, cancelAnimationFrame() {},
  matchMedia() { return { matches: true }; },
  getComputedStyle() { return { paddingBottom: "0" }; },
  addEventListener() {},
  t(key) { return key; },
};
context.window = context;
context.globalThis = context;
context.parent = context;
vm.createContext(context);
for (const file of ["../core/app.js", "../games/chess.js", "../games/spyfall.js"])
  vm.runInContext(fs.readFileSync(new URL(file, import.meta.url), "utf8"), context);

const A = context.A;
A.sfx = () => {};
A.vibe = () => {};
A.joined = false;
function fire(ms) {
  for (const timer of intervals.filter((x) => x.active && x.ms === ms)) timer.fn();
}

const chessBase = {
  t: "chess", phase: "playing", board: ".".repeat(64), moves: [], white: true,
  wtm: true, yourTurn: true, remaining_ms: 90000, other_remaining_ms: 120000,
  duration_ms: 300000, paused: false, last: -1, check: false, offer: 0,
  claim3: false, claim50: false, you: 1, opp: "ORBIT",
};
A.view = "chess";
A.handlers.chess(chessBase);
assert.equal(elements.get("chess-my-clock").textContent, "1:30");
assert.equal(elements.get("chess-opp-clock").textContent, "2:00");
mono += 10000;
wall += 50_000_000;
fire(200);
assert.equal(elements.get("chess-my-clock").textContent, "1:20", "my active clock is monotonic");
assert.equal(elements.get("chess-opp-clock").textContent, "2:00", "waiting clock stays frozen");

A.handlers.chess({ ...chessBase, remaining_ms: 80000, paused: true });
mono += 20000;
fire(200);
assert.equal(elements.get("chess-my-clock").textContent, "1:20", "match pause freezes Chess");
assert.equal(elements.get("chess-opp-clock").textContent, "2:00");

A.handlers.chess({
  ...chessBase, wtm: false, yourTurn: false, remaining_ms: 120000,
  other_remaining_ms: 80000, paused: false,
});
mono += 10000;
fire(200);
assert.equal(elements.get("chess-my-clock").textContent, "1:20");
assert.equal(elements.get("chess-opp-clock").textContent, "1:50", "opponent clock runs on their turn");

A.handlers.chess({
  ...chessBase, phase: "over", result: "draw", reason: "agree",
  wtm: false, yourTurn: false, remaining_ms: 110000, other_remaining_ms: 80000, paused: false,
});
mono += 30000;
fire(200);
assert.equal(elements.get("chess-my-clock").textContent, "1:20", "over clocks are static");
assert.equal(elements.get("chess-opp-clock").textContent, "1:50");

const spyBase = {
  t: "spyfall", phase: "play", stage: "talk", me: false, round: 1, rounds: 3,
  remaining_ms: 360000, duration_ms: 360000, paused: false,
};
A.view = "spyfall";
A.handlers.spyfall(spyBase);
assert.equal(elements.get("sf-clock").textContent, "6:00");
mono += 10000;
wall -= 100_000_000;
fire(200);
assert.equal(elements.get("sf-clock").textContent, "5:50", "Spyfall ignores wall-clock jumps");

A.handlers.spyfall({ ...spyBase, remaining_ms: 350000, paused: true });
mono += 60000;
fire(200);
assert.equal(elements.get("sf-clock").textContent, "5:50", "Spyfall pause freezes its clock");
A.handlers.spyfall({ ...spyBase, remaining_ms: 350000, paused: false });
mono += 10000;
fire(200);
assert.equal(elements.get("sf-clock").textContent, "5:40", "Spyfall resumes from a fresh snapshot");

console.log("web Chess and Spyfall monotonic clocks: OK");
