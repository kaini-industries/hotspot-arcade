import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

function element() {
  const classes = new Set(["hide"]);
  return {
    textContent: "", value: "", className: "",
    classList: {
      add: (x) => classes.add(x), remove: (x) => classes.delete(x),
      toggle: (x, on) => on ? classes.add(x) : classes.delete(x),
      contains: (x) => classes.has(x),
    },
    focus() {}, setAttribute() {}, querySelector() { return null; },
  };
}

const elements = new Map();
const document = {
  hidden: false,
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  },
  querySelectorAll() { return []; },
  addEventListener() {},
};
const sockets = [];
class FakeWebSocket {
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    sockets.push(this);
  }
  send() {}
  close() {}
}
const timers = [];
const random = [
  0x00, 0x01, 0x0a, 0x0f, 0x10, 0x20, 0x7f, 0x80,
  0xa0, 0xfe, 0xff, 0x05, 0x09, 0x11, 0x22, 0x33,
];
const context = {
  __HA_TEST__: true,
  document,
  navigator: {},
  location: { protocol: "http:", host: "192.168.4.1", hostname: "192.168.4.1", search: "" },
  URLSearchParams,
  Uint8Array,
  WebSocket: FakeWebSocket,
  crypto: { getRandomValues(bytes) { bytes.set(random); return bytes; } },
  console,
  setTimeout(fn, ms) { timers.push({ fn, ms }); return timers.length; },
  clearTimeout() {}, setInterval() { return 1; }, clearInterval() {},
  requestAnimationFrame(fn) { fn(); return 1; }, cancelAnimationFrame() {},
  getComputedStyle() { return { transform: "none" }; },
  t: (key) => key === "net.identity_takeover"
    ? "This player is active in another tab. Reload to use this tab."
    : key,
};
context.window = context;
context.globalThis = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(new URL("../core/app.js", import.meta.url), "utf8"), context);

assert.equal(
  context.__HA_TEST_API__.createResumeToken(),
  "00010a0f10207f80a0feff0509112233",
  "all random bytes are formatted as two lowercase hex digits without padStart",
);

context.A.view = "lobby";
context.A.joined = true;
context.A.authenticated = true;
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 1);
sockets[0].onclose({ code: 1008, reason: "identity takeover" });
assert.equal(context.A.takeover, true);
assert.equal(context.A.authenticated, false);
assert.equal(timers.length, 0, "a displaced tab must not automatically take the identity back");
assert.equal(
  elements.get("netbar").textContent,
  "This player is active in another tab. Reload to use this tab.",
  "the displaced tab shows a useful terminal state",
);
assert.equal(elements.get("netbar").classList.contains("hide"), false);
assert.match(elements.get("dot").className, /\bbad\b/);
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 1, "queued/manual reconnect attempts remain blocked until reload");

// A transport failure with any other close code remains recoverable.
context.A.takeover = false;
context.A.authenticated = true;
context.__HA_TEST_API__.connect();
assert.equal(sockets.length, 2);
sockets[1].onclose({ code: 1006, reason: "" });
assert.equal(context.A.takeover, false);
assert.equal(timers.length, 1, "ordinary network loss still schedules reconnect");
assert.equal(timers[0].ms, 1000);

console.log("web socket reconnect policy: OK");
