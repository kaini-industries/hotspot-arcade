import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const e = await newEngine();
e.reset();

// Protocol v2 is explicit. An old cached client cannot silently allocate a seat.
let out = e.inputRaw(99, '{"t":"hello","nick":"OLD"}');
assert.equal(lastToWs(out, 99, "reject").msg.code, "bad_protocol");

const token = "0123456789abcdef0123456789abcdef";
const identity = createHash("sha256").update(token).digest("hex").slice(0, 32);
out = e.input(98, { t: "hello", proto: 2, nick: "NO CODE", avatar: "🙂", resume: token });
assert.equal(lastToWs(out, 98, "reject").msg.code, "auth_required");
out = e.input(98, {
  t: "hello", proto: 2, nick: "BAD CODE", avatar: "🙂", resume: token, code: "654321",
});
assert.equal(lastToWs(out, 98, "reject").msg.code, "bad_code");
e.setAdmissionFull(true);
out = e.join(97, "FULL", "97979797979797979797979797979797");
assert.equal(lastToWs(out, 97, "reject").msg.code, "full");
e.setAdmissionFull(false);
out = e.input(96, {
  t: "hello", proto: 2, nick: "LONG", avatar: "🙂", resume: token, code: "1234567",
});
assert.equal(lastToWs(out, 96, "reject").msg.code, "bad_code");

const first = e.join(1, "ALICE", token);
const welcome = lastToWs(first, 1, "welcome").msg;
assert.equal(welcome.proto, 2);
assert.equal(welcome.pid, 1);
assert.equal(welcome.resumed, false);
assert.match(welcome.session, /^[0-9a-f]{32}$/);
assert.equal("resume" in welcome, false, "raw token is never echoed");
const stableJoin = first.find((x) => x.to === "uart" && x.kind === "join" && x.pid === 1);
assert.equal(stableJoin.identity, identity);
assert.equal(JSON.stringify(first).includes(token), false, "raw token never enters host output");

e.join(2, "BOB");
out = e.disconnect(1);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
let roster = lastToWs(out, 2, "lobby").msg.players;
assert.equal(roster.find((p) => p.pid === 1).online, false);

// At 119,999ms the token reclaims the same seat and score/state identity.
e.tick(119999);
out = e.join(3, "ALICE", token);
assert.equal(lastToWs(out, 3, "welcome").msg.pid, 1);
assert.equal(lastToWs(out, 3, "welcome").msg.resumed, true);

// A duplicate live token is a deterministic takeover. The old socket is closed
// and, because it no longer owns the pid, cannot act or detach the new socket.
out = e.join(4, "ALICE", token);
assert.equal(lastToWs(out, 4, "welcome").msg.pid, 1);
const close = out.find((x) => x.to === "ws" && x.id === 3 && x.kind === "close");
assert.equal(close.code, 1008);
assert.equal(close.reason, "identity takeover");
assert.deepEqual(e.input(3, { t: "react", emoji: "🎉" }), []);
assert.deepEqual(e.disconnect(3), []);

// At exactly 120 seconds detached, the engine finalizes once. The host still
// recognizes the digest, so the credential can obtain a fresh seat without code.
e.disconnect(4);
out = e.tick(239998);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
out = e.tick(239999);
assert.equal(out.filter((x) => x.to === "uart" && x.kind === "leave" && x.pid === 1).length, 1);
out = e.join(5, "ALICE", token, null);
assert.equal(lastToWs(out, 5, "welcome").msg.resumed, false);
assert.equal(lastToWs(out, 5, "welcome").msg.pid, 1);

// Hello itself enforces the boundary when loop() has not ticked.
const boundary = await newEngine();
boundary.resetAt(0);
const boundaryToken = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
boundary.join(1, "BOUNDARY", boundaryToken);
boundary.disconnect(1);
out = boundary.inputAt(2, {
  t: "hello", proto: 2, nick: "BOUNDARY", avatar: "🙂", resume: boundaryToken,
}, 120000);
assert.equal(lastToWs(out, 2, "welcome").msg.resumed, false);

assert.equal(e.timeReached(0xfffffff0, 0x20), false);
assert.equal(e.timeRemaining(0xfffffff0, 0x20), 48);
assert.equal(e.timeReached(0x20, 0x20), true);

console.log("identity: protocol v2 checks passed");
