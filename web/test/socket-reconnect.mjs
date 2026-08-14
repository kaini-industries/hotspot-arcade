import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

function element() {
  const classes = new Set(["hide"]);
  const listeners = new Map();
  return {
    textContent: "", value: "", className: "", innerHTML: "", focused: false,
    children: [], disabled: false,
    classList: {
      add: (x) => classes.add(x), remove: (x) => classes.delete(x),
      toggle: (x, on) => on ? classes.add(x) : classes.delete(x),
      contains: (x) => classes.has(x),
    },
    focus() { this.focused = true; },
    setAttribute() {}, querySelector() { return null; },
    addEventListener(type, fn) { listeners.set(type, fn); },
    appendChild(child) { this.children.push(child); return child; },
    click() { const fn = listeners.get("click"); if (fn) fn({ target: this }); },
  };
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
  querySelectorAll() { return []; },
  addEventListener() {},
};
const sockets = [];
class FakeWebSocket {
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    this.closeCount = 0;
    sockets.push(this);
  }
  send(value) { this.sent.push(JSON.parse(value)); }
  close() { this.closeCount++; }
}
const timers = [];
const intervals = [];
let now = 1000;
function activeTimers() { return timers.filter((x) => x.active); }
function activeIntervals() { return intervals.filter((x) => x.active); }
function fire(timer) { timer.active = false; timer.fn(); }
const random = [
  0x00, 0x01, 0x0a, 0x0f, 0x10, 0x20, 0x7f, 0x80,
  0xa0, 0xfe, 0xff, 0x05, 0x09, 0x11, 0x22, 0x33,
];
const context = {
  __HA_TEST__: true,
  document,
  navigator: { wakeLock: { request() { throw new Error("not used"); } } },
  location: { protocol: "http:", host: "192.168.4.1", hostname: "192.168.4.1", search: "" },
  URLSearchParams,
  Uint8Array,
  WebSocket: FakeWebSocket,
  crypto: { getRandomValues(bytes) { bytes.set(random); return bytes; } },
  console,
  Date: { now() { return now; } },
  performance: { now() { return now; } },
  setTimeout(fn, ms) {
    const timer = { fn, ms, active: true };
    timers.push(timer);
    return timer;
  },
  clearTimeout(timer) { if (timer) timer.active = false; },
  setInterval(fn, ms) {
    const timer = { fn, ms, active: true };
    intervals.push(timer);
    return timer;
  },
  clearInterval(timer) { if (timer) timer.active = false; },
  requestAnimationFrame(fn) { fn(); return 1; }, cancelAnimationFrame() {},
  getComputedStyle() { return { transform: "none" }; },
  t: (key) => key,
};
context.window = context;
context.globalThis = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(new URL("../core/app.js", import.meta.url), "utf8"), context);

assert.equal(
  context.__HA_TEST_API__.createResumeToken(),
  "00010a0f10207f80a0feff0509112233",
  "all random bytes are formatted as two lowercase hex digits",
);

context.A.view = "lobby";
context.A.joined = true;
context.A.nick = "NOVA";
context.A.avatar = "🙂";
context.A.resume = "0123456789abcdef0123456789abcdef";
context.A.code = "123456";
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 1);
sockets[0].readyState = 1;
sockets[0].onopen();
assert.deepEqual(sockets[0].sent[0], {
  t: "hello", proto: 2, nick: "NOVA", avatar: "🙂",
  resume: "0123456789abcdef0123456789abcdef", code: "123456",
});

const live = activeIntervals()[0];
assert.equal(live.ms, 2000, "liveness checks run at the protocol interval");
now += 5001;
live.fn();
assert.match(elements.get("dot").className, /\bwarn\b/, "quiet links warn before closing");
assert.equal(elements.get("netbar").textContent, "net.quiet");
assert.deepEqual(sockets[0].sent.at(-1), { t: "ping" }, "liveness sends an active probe");
sockets[0].onmessage({ data: JSON.stringify({ t: "pong" }) });
live.fn();
assert.doesNotMatch(elements.get("dot").className, /\bwarn\b/, "received traffic clears warning");

context.__HA_TEST_API__.dispatch({
  t: "welcome", proto: 2, session: "f".repeat(32), pid: 2,
  resumed: false, lang: "",
});
assert.equal(context.A.authenticated, true);
assert.equal(context.A.pid, 2);
assert.equal(context.A.code, "", "the admission code is discarded after welcome");

sockets[0].onclose({ code: 1008, reason: "identity takeover" });
assert.equal(context.A.takeover, true);
assert.equal(context.A.authenticated, false);
assert.equal(activeTimers().length, 0, "a displaced tab must not automatically take the identity back");
assert.equal(elements.get("netbar").textContent, "net.identity_takeover");
assert.equal(elements.get("netbar").classList.contains("hide"), false);
assert.match(elements.get("dot").className, /\bbad\b/);
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 1, "reconnect attempts remain blocked until reload");

// Any other close code remains recoverable.
context.A.takeover = false;
context.A.authenticated = true;
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 2);
sockets[1].onclose({ code: 1006, reason: "" });
assert.equal(context.A.takeover, false);
assert.equal(activeTimers().length, 1, "ordinary network loss still schedules reconnect");
assert.equal(activeTimers()[0].ms, 1000);

// A WebSocket that remains in CONNECTING may never emit open/error/close. Its bounded
// attempt timer must retire it and enter the same single reconnect path.
for (const timer of timers) timer.active = false;
context.A.retry = 0;
context.__HA_TEST_API__.connect();
const stuck = sockets.at(-1);
const connectTimeout = activeTimers().find((x) => x.ms === 10000);
assert.ok(connectTimeout, "each connecting socket has a bounded attempt timer");
fire(connectTimeout);
assert.equal(stuck.closeCount, 1, "a stuck connecting socket is closed");
assert.equal(context.A.ws, null, "the stuck socket is retired even without onclose");
assert.equal(activeTimers().length, 1, "exactly one reconnect is scheduled");
assert.equal(activeTimers()[0].ms, 1000);

// Typed rejects return to admission without mutating the browser credential.
for (const timer of timers) timer.active = false;
context.A.resume = "0123456789abcdef0123456789abcdef";
context.A.joined = true;
context.__HA_TEST_API__.dispatch({ t: "reject", code: "bad_code" });
assert.equal(context.A.authenticated, false);
assert.equal(context.A.joined, false);
assert.equal(context.A.view, "landing");
assert.equal(elements.get("join-code").focused, true);
assert.equal(elements.get("toast").textContent, "join.bad_code");
assert.equal(context.A.resume, "0123456789abcdef0123456789abcdef");

context.__HA_TEST_API__.dispatch({ t: "error", code: "match_capacity" });
assert.equal(elements.get("toast").textContent, "error.match_capacity");
context.__HA_TEST_API__.dispatch({ t: "error", code: "challenge_capacity" });
assert.equal(elements.get("toast").textContent, "error.challenge_capacity");
context.__HA_TEST_API__.dispatch({ t: "error", code: "future_error" });
assert.equal(elements.get("toast").textContent, "error.unknown");

// Challenge acceptance is bound to the server-assigned ID, not the challenger's
// PID. A stale acceptance must not consume a later challenge from that player.
context.A.ws = sockets[1];
sockets[1].readyState = 1;
context.A.pid = 2;
context.A.players = [
  { pid: 2, nick: "NOVA", online: true },
  { pid: 3, nick: "ORBIT", online: true },
];
context.A.sfx = () => {};
const incoming = element();
const players = element();
context.__HA_TEST_API__.lobbyView(incoming, players, [
  { id: 77, from: 3, to: 2 },
]);
assert.equal(incoming.children.length, 1);
assert.equal(incoming.children[0].children.length, 2);
incoming.children[0].children[0].click();
assert.deepEqual(sockets[1].sent.at(-1), { t: "accept", id: 77 });

// A committed locale reaches already-connected phones, and a phone game request is
// answered as policy—not with the legacy vote overlay or a local game mutation.
let configuredLang = null;
context.A.setLang = (lang) => { configuredLang = lang; };
context.__HA_TEST_API__.dispatch({ t: "config", lang: "de" });
assert.equal(configuredLang, "de");
context.__HA_TEST_API__.dispatch({
  t: "result", event: "game_change", status: "policy_denied", game: "wyr", id: 8,
});
assert.equal(elements.get("toast").textContent, "gamevote.host_only");

// A planned-downtime overlay survives the actual close/reconnect path. Generic
// reconnect UI stays behind it; resume/config/keepalive cannot dismiss it.
for (const timer of timers) timer.active = false;
context.A.takeover = false;
context.A.joined = true;
context.A.view = "lobby";
context.__HA_TEST_API__.connect();
const planned = sockets.at(-1);
planned.readyState = 1;
planned.onopen();
context.__HA_TEST_API__.dispatch({
  t: "server_pause", reason: "ssid_change", ssid: "ARCADE-NEW", reconnect_ms: 600000,
});
assert.equal(elements.get("transport").classList.contains("hide"), false);
planned.onclose({ code: 1006, reason: "" });
assert.equal(elements.get("transport").classList.contains("hide"), false);
assert.equal(elements.get("netbar").classList.contains("hide"), true);
const retry = activeTimers().find((x) => x.ms === 1000);
assert.ok(retry, "planned socket loss still retries in the background");
fire(retry);
const returned = sockets.at(-1);
returned.readyState = 1;
returned.onopen();
context.__HA_TEST_API__.dispatch({ t: "welcome", pid: 2, lang: "de" });
assert.equal(context.A.transportRestoring, true);
assert.equal(elements.get("transport").classList.contains("hide"), false);
context.__HA_TEST_API__.dispatch({ t: "server_pause", reason: "ssid_change", ssid: "ARCADE-NEW", reconnect_ms: 600000 });
assert.equal(context.A.transportPaused, true, "a host still paused reasserts the pause after welcome");
context.__HA_TEST_API__.dispatch({ t: "server_resume" });
context.__HA_TEST_API__.dispatch({ t: "config", lang: "de" });
context.__HA_TEST_API__.dispatch({ t: "pong" });
assert.equal(elements.get("transport").classList.contains("hide"), false);
context.__HA_TEST_API__.dispatch({ t: "trivia", phase: "lobby", paused: false });
assert.equal(elements.get("transport").classList.contains("hide"), true);

console.log("web protocol-v2 reconnect policy: OK");
