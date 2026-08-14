import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

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

// Challenge acceptance is bound to a server-issued id. Detaching either endpoint
// removes the invitation immediately, so an old id cannot start a later match.
const stale = await newEngine();
stale.reset();
stale.selectGame(2);
const staleToken = "cccccccccccccccccccccccccccccccc";
stale.join(1, "CHALLENGER", staleToken);
stale.join(2, "TARGET");
const staleInvite = stale.input(1, { t: "challenge", to: 2 });
const staleId = challengeId(staleInvite, 2);
assert.ok(Number.isInteger(staleId) && staleId > 0);
assert.deepEqual(stale.input(1, { t: "challenge", to: 258 }), [],
  "out-of-range challenge pids cannot wrap onto a real seat");
assert.deepEqual(stale.input(1, { t: "challenge", to: -254 }), [],
  "negative challenge pids cannot wrap onto a real seat");
stale.tick(1000);
stale.disconnect(1);
assert.deepEqual(stale.input(2, { t: "accept", id: staleId }), []);
stale.inputAt(3, {
  t: "hello", proto: 2, nick: "CHALLENGER", avatar: "🙂", resume: staleToken,
}, 2000);
const laterInvite = stale.input(3, { t: "challenge", to: 2 });
const laterId = challengeId(laterInvite, 2);
assert.ok(Number.isInteger(laterId) && laterId > 0 && laterId !== staleId,
  "a later invitation to the same endpoint gets a distinct id");
assert.deepEqual(stale.input(2, { t: "accept", id: staleId }), [],
  "the stale id cannot consume the later invitation");
out = stale.input(2, { t: "accept", id: laterId });
assert.equal(lastToWs(out, 2, "duel").msg.phase, "playing",
  "only the new invitation id reserves the match");

// Exhausting a fixed match table reports capacity to both players and leaves
// the challenge available for a later retry. Pong has four slots by default.
const cap = await newEngine();
cap.reset();
for (let ws = 1; ws <= 10; ws++) cap.join(ws, "M" + ws);
cap.selectGame(6);
for (let ws = 1; ws <= 7; ws += 2) {
  const invite = cap.input(ws, { t: "challenge", to: ws + 1 });
  cap.input(ws + 1, { t: "accept", id: challengeId(invite, ws + 1) });
}
const overflowInvite = cap.input(9, { t: "challenge", to: 10 });
const overflowId = challengeId(overflowInvite, 10);
out = cap.input(10, { t: "accept", id: overflowId });
assert.equal(lastToWs(out, 9, "error").msg.code, "match_capacity");
assert.equal(lastToWs(out, 10, "error").msg.code, "match_capacity");
assert.equal(challengeId(out, 10), overflowId, "capacity failure keeps the invite retryable");

// A simultaneous timeout is a double forfeit, not a score decided by ascending
// pid iteration order.
const doubleExpiry = await newEngine();
doubleExpiry.resetAt(0);
doubleExpiry.join(1, "LEFT");
doubleExpiry.join(2, "RIGHT");
doubleExpiry.selectGame(6);
const doubleInvite = doubleExpiry.input(1, { t: "challenge", to: 2 });
doubleExpiry.input(2, { t: "accept", id: challengeId(doubleInvite, 2) });
doubleExpiry.disconnect(1);
doubleExpiry.disconnect(2);
out = doubleExpiry.tick(120000);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "score"), false,
  "two expiring opponents cannot win by forfeit against each other");

// The simulator's quorum override is still useful to an admitted player, but a
// pending/unknown socket cannot alter it before hello.
const authz = await newEngine();
authz.reset();
assert.deepEqual(authz.input(99, { t: "minoverride", on: true }), []);
out = authz.join(1, "AUTHED");
assert.equal(lastToWs(out, 1, "lobby").msg.minoverride, false);

console.log("identity: protocol v2 checks passed");
