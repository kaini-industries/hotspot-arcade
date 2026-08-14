import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

function classList(initial = ["hide"]) {
  const values = new Set(initial);
  return {
    add(x) { values.add(x); },
    remove(x) { values.delete(x); },
    toggle(x, on) {
      if (on === undefined) on = !values.has(x);
      on ? values.add(x) : values.delete(x);
      return on;
    },
    contains(x) { return values.has(x); },
  };
}

function element() {
  const fill = { style: {}, classList: classList([]) };
  return {
    textContent: "", value: "", className: "", innerHTML: "", style: {}, paused: true,
    firstElementChild: fill, children: [], disabled: false,
    classList: classList(),
    appendChild(child) { this.children.push(child); return child; },
    removeChild(child) { this.children.splice(this.children.indexOf(child), 1); },
    addEventListener() {}, setAttribute() {}, focus() {},
    play() { this.paused = false; return { catch() {} }; },
    pause() { this.paused = true; },
  };
}

const elements = new Map();
const body = element();
const document = {
  hidden: false,
  body,
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
let wall = 1_000_000;
const intervals = [];
const timeouts = [];
function activeIntervals(ms) {
  return intervals.filter((x) => x.active && (ms === undefined || x.ms === ms));
}
function fireIntervals(ms) {
  for (const timer of activeIntervals(ms)) timer.fn();
}

const sent = [];
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
  crypto: { getRandomValues(bytes) { bytes.fill(1); return bytes; } },
  localStorage: { getItem() { return null; }, setItem() {} },
  isFinite,
  setInterval(fn, ms) {
    const timer = { fn, ms, active: true };
    intervals.push(timer);
    return timer;
  },
  clearInterval(timer) { if (timer) timer.active = false; },
  setTimeout(fn, ms) {
    const timer = { fn, ms, active: true };
    timeouts.push(timer);
    return timer;
  },
  clearTimeout(timer) { if (timer) timer.active = false; },
  requestAnimationFrame(fn) { fn(); return 1; },
  cancelAnimationFrame() {},
  matchMedia() { return { matches: true }; },
  t(key, params) {
    let out = key;
    for (const name of Object.keys(params || {})) out += ` ${name}=${params[name]}`;
    return out;
  },
};
context.window = context;
context.globalThis = context;
context.parent = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(new URL("../core/app.js", import.meta.url), "utf8"), context);

const A = context.A;
const api = context.__HA_TEST_API__;
A.sfx = () => {};
A.vibe = () => {};
assert.equal(api.authoritativeState({ t: "frankendraw", phase: "draw" }), true);
assert.equal(api.authoritativeState({ t: "fdart" }), false);
assert.equal(api.authoritativeState({ t: "ink" }), false);
assert.equal(api.authoritativeState({ t: "pong" }), false);
assert.equal(api.authoritativeState({ t: "pong", phase: "playing" }), true);

// Relative timers advance only from the monotonic clock and clamp bad snapshots.
let snap = A.timerSnapshot(8000, 10000, false);
mono += 1250;
wall += 86_400_000;
assert.equal(A.timerRemaining(snap), 6750, "wall-clock jumps do not change a game timer");
assert.equal(A.timerSnapshot(12000, 10000, false).remaining, 10000, "remaining clamps to duration");
assert.equal(A.timerSnapshot(Infinity, 10000, false).remaining, 0, "non-finite remaining is rejected");
const paused = A.timerSnapshot(7000, 10000, true);
mono += 5000;
assert.equal(A.timerRemaining(paused), 7000, "paused snapshots stay frozen");

// Bars render from those snapshots and freeze at the exact planned-pause instant.
mono = 1000;
A.timebar("test-bar", 8000, 10000, false, false);
assert.equal(elements.get("test-bar").firstElementChild.style.transform, "scaleX(0.8)");
mono = 3000;
fireIntervals(100);
assert.equal(elements.get("test-bar").firstElementChild.style.transform, "scaleX(0.6)");

api.dispatch({
  t: "server_pause", reason: "ssid_change", ssid: "ARCADE-NEW", reconnect_ms: 600000,
});
assert.equal(A.transportBlocked, true);
assert.equal(A.clockFrozen, true);
assert.equal(elements.get("transport").classList.contains("hide"), false);
assert.equal(elements.get("transport-title").textContent, "transport.title");
assert.match(elements.get("transport-ssid").textContent, /ARCADE-NEW/);
assert.equal(elements.get("netbar").classList.contains("hide"), true);

mono += 600000;
wall -= 10_000_000;
fireIntervals(100);
assert.equal(
  elements.get("test-bar").firstElementChild.style.transform,
  "scaleX(0.6)",
  "ten minutes of AP downtime consume no local timer",
);

api.dispatch({ t: "server_resume" });
assert.equal(A.transportRestoring, true);
assert.equal(A.clockFrozen, true);
assert.equal(elements.get("transport-title").textContent, "transport.restoring_title");
api.dispatch({ t: "config", lang: "de" });
assert.equal(A.transportBlocked, true, "config is not an authoritative state");
api.dispatch({ t: "pong" });
assert.equal(A.transportBlocked, true, "a keepalive pong cannot complete restoration");

A.handlers.trivia = () => {};
api.dispatch({ t: "trivia", phase: "question", paused: false });
assert.equal(A.transportBlocked, false);
assert.equal(A.clockFrozen, false);
assert.equal(elements.get("transport").classList.contains("hide"), true);
assert.equal(
  elements.get("test-bar").firstElementChild.style.transform,
  "scaleX(0.6)",
  "restoration itself consumes no timer",
);
mono += 1000;
fireIntervals(100);
assert.equal(elements.get("test-bar").firstElementChild.style.transform, "scaleX(0.5)");

// Planned transport accepts only recovery traffic. A role/match pause additionally
// allows chat, emoji, explicit leave, and chess resignation.
A.ws = { readyState: 1, send(value) { sent.push(JSON.parse(value)); } };
api.dispatch({ t: "server_pause", reason: "ap_off", reconnect_ms: 0 });
api.send({ t: "say", text: "blocked" });
api.send({ t: "move", n: 2 });
api.send({ t: "ping" });
assert.deepEqual(sent.splice(0), [{ t: "ping" }]);
api.dispatch({ t: "server_resume" });
api.dispatch({ t: "trivia", phase: "question", paused: true });
assert.equal(A.gamePaused, true);
assert.equal(elements.get("game-pause").textContent, "", "pause copy remains markup/i18n-owned");
api.send({ t: "move", n: 3 });
api.send({ t: "say", text: "safe" });
api.send({ t: "react", emoji: "👍" });
api.send({ t: "leaveGame" });
api.send({ t: "resign" });
assert.deepEqual(sent.splice(0), [
  { t: "say", text: "safe" }, { t: "react", emoji: "👍" },
  { t: "leaveGame" }, { t: "resign" },
]);

// A typed paused reject preserves the browser identity and uses the same durable
// overlay. If server_resume was missed, welcome moves it to Restoring and only the
// following authoritative state clears it.
A.joined = true;
api.dispatch({ t: "reject", code: "server_paused", retry_ms: 600000 });
assert.equal(A.joined, true);
assert.equal(A.transportPaused, true);
api.dispatch({ t: "server_resume" });
assert.equal(sent.at(-1).t, "hello", "a paused unauthenticated player retries admission on resume");
api.dispatch({ t: "welcome", pid: 4, lang: "" });
assert.equal(A.transportRestoring, true);
api.dispatch({ t: "config", lang: "" });
assert.equal(A.transportBlocked, true);
api.dispatch({ t: "trivia", phase: "lobby", paused: false });
assert.equal(A.transportBlocked, false);

A.joined = true;
api.dispatch({ t: "reject", code: "server_paused", retry_ms: 600000 });
api.dispatch({ t: "server_resume" });
api.dispatch({ t: "reject", code: "bad_code" });
assert.equal(A.transportBlocked, false, "a failed post-resume admission cannot strand the overlay");
assert.equal(A.view, "landing");

console.log("web monotonic timer and transport pause policy: OK");
