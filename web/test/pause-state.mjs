import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

function element() {
  const classes = new Set(["hide"]);
  return {
    textContent: "", value: "",
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
const context = {
  __HA_TEST__: true,
  document,
  navigator: {},
  location: { protocol: "http:", host: "192.168.4.1", hostname: "192.168.4.1", search: "" },
  URLSearchParams,
  console,
  setTimeout() { return 1; }, clearTimeout() {}, setInterval() { return 1; }, clearInterval() {},
  requestAnimationFrame(fn) { fn(); return 1; }, cancelAnimationFrame() {},
  getComputedStyle() { return { transform: "none" }; },
  t: (key) => key,
};
context.globalThis = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(new URL("../core/app.js", import.meta.url), "utf8"), context);

const sent = [];
context.A.ws = { readyState: 1, send: (v) => sent.push(JSON.parse(v)) };
context.A.joined = true;
context.A.authenticated = false;
context.A.nick = "NOVA";
context.A.avatar = "🙂";
context.A.resume = "0123456789abcdef0123456789abcdef";
context.A.code = "123456";

context.__HA_TEST_API__.dispatch({
  t: "server_pause", reason: "ssid_change", ssid: "Arcade", reconnect_ms: 600000,
});
assert.equal(sent.length, 0);
context.__HA_TEST_API__.dispatch({ t: "server_resume" });
assert.deepEqual(sent, [{
  t: "hello", proto: 2, nick: "NOVA", avatar: "🙂",
  resume: "0123456789abcdef0123456789abcdef", code: "123456",
}], "a not-yet-welcomed socket retries hello after planned resume");

sent.length = 0;
context.__HA_TEST_API__.dispatch({
  t: "welcome", proto: 2, session: "f".repeat(32), pid: 2, nick: "NOVA",
  avatar: "🙂", lang: "", resumed: false, paused: false,
});
context.__HA_TEST_API__.dispatch({ t: "server_resume" });
assert.equal(sent.length, 0, "an already authenticated socket does not duplicate hello");

console.log("web pause state: OK");
