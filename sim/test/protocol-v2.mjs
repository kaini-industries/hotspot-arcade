import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

const C4 = 2;
const PONG = 6;
const e = await newEngine();
e.reset();

// Protocol v2 is explicit: an old cached client cannot silently allocate a seat.
let out = e.inputRaw(99, '{"t":"hello","nick":"OLD"}');
assert.equal(lastToWs(out, 99, "reject").msg.code, "bad_protocol");

const token = "0123456789abcdef0123456789abcdef";
const identity = createHash("sha256").update(token).digest("hex").slice(0, 32);
out = e.input(98, { t: "hello", proto: 2, nick: "NO CODE", avatar: "🙂", resume: token });
assert.equal(lastToWs(out, 98, "reject").msg.code, "auth_required");
out = e.input(98, { t: "hello", proto: 2, nick: "BAD CODE", avatar: "🙂", resume: token, code: "654321" });
assert.equal(lastToWs(out, 98, "reject").msg.code, "bad_code");
e.setAdmissionFull(true);
out = e.input(96, { t: "hello", proto: 2, nick: "LEDGER FULL", avatar: "🙂", resume: "96969696969696969696969696969696", code: "123456" });
assert.equal(lastToWs(out, 96, "reject").msg.code, "full", "host ledger capacity is an explicit full rejection");
e.setAdmissionFull(false);
out = e.input(97, { t: "hello", proto: 2, nick: "LONG CODE", avatar: "🙂", resume: token, code: "1234567" });
assert.equal(lastToWs(out, 97, "reject").msg.code, "bad_code", "overlong code must not authenticate by truncation");

const first = e.join(1, "ALICE", token);
const welcome = lastToWs(first, 1, "welcome").msg;
assert.equal(welcome.proto, 2);
assert.equal(welcome.pid, 1);
assert.equal(welcome.resumed, false);
assert.match(welcome.session, /^[0-9a-f]{32}$/);
assert.equal("resume" in welcome, false, "raw browser token is never echoed by the server");
const stableJoin = first.find((x) => x.to === "uart" && x.kind === "join" && x.pid === 1);
assert.equal(stableJoin.identity, identity);
assert.equal(JSON.stringify(first).includes(token), false, "raw token never enters host output");
out = e.input(1, { t: "say", text: "é".repeat(55) });
const boundedEvent = out.find((x) => x.to === "uart" && x.kind === "host_event");
assert.ok(boundedEvent, "chat produces a typed host event");
assert.ok(Buffer.byteLength(boundedEvent.event.text, "utf8") <= 96, "host event text is wire bounded");
assert.doesNotThrow(() => new TextDecoder("utf-8", { fatal: true }).decode(Buffer.from(boundedEvent.event.text)),
  "host event truncation preserves valid UTF-8");
for (const malformed of [
  '{"t":"say","text":"unterminated}',
  '{"t":"say","text":"bad\\qescape"}',
  '{"t":"sayyyyyyyyyyyyyyyyyyyyyyyyy":"say","text":"hidden"}',
  '{"t":"say","text":"prefix"}garbage',
  '{"nested":{"t":"say"},"text":"smuggled"}',
  '{"t":"say",}',
]) assert.deepEqual(e.inputRaw(1, malformed), [], "malformed/overlong strings are rejected without a prefix parse");
e.join(2, "BOB");
e.selectGame(C4);

for (const raw of [
  '{"t":"challenge","to":9999999999999999999999999999999999999999999999999999999999999999999999}',
  '{"t":"challenge","to":1oops}',
  '{"t":"challenge","to":257}',
]) assert.deepEqual(e.inputRaw(2, raw), [], "overflow, malformed, and narrowing integers are rejected");

// Challenges are identities of their own, not ambiguous `from` lookups. A
// disconnect clears them immediately, while the player seat remains reserved.
const challenged = e.input(1, { t: "challenge", to: 2 });
const staleId = challengeId(challenged, 2);
assert.ok(Number.isInteger(staleId) && staleId > 0);
out = e.disconnect(1);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
const lobby = lastToWs(out, 2, "lobby").msg;
assert.equal(lobby.players.find((p) => p.pid === 1).online, false);
assert.deepEqual(lastToWs(out, 2, "duel").msg.challenges, []);
assert.equal(e.input(2, { t: "accept", id: staleId }).length, 0, "stale id cannot start a match");

// Just before expiry the same opaque token rebinds the same pid.
out = e.tick(119999);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
out = e.join(3, "ALICE", token);
const resumed = lastToWs(out, 3, "welcome").msg;
assert.equal(resumed.pid, 1);
assert.equal(resumed.resumed, true);
assert.equal(lastToWs(out, 3, "lobby").msg.players.find((p) => p.pid === 1).online, true);

// The bearer token may be presented while its previous socket is still alive.
// The newest binding wins; the stale socket can no longer act as this player.
out = e.join(4, "ALICE", token);
assert.equal(lastToWs(out, 4, "welcome").msg.pid, 1);
assert.equal(lastToWs(out, 4, "welcome").msg.resumed, true);
assert.ok(out.some((x) => x.to === "ws" && x.id === 3 && x.kind === "close"), "takeover closes the stale socket");
assert.deepEqual(e.input(3, { t: "challenge", to: 2 }), []);

const challengedAgain = e.input(4, { t: "challenge", to: 2 });
const liveId = challengeId(challengedAgain, 2);
assert.notEqual(liveId, staleId, "challenge ids are not reused immediately");
out = e.input(2, { t: "accept", id: liveId });
assert.equal(lastToWs(out, 4, "duel").msg.phase, "playing");

// Win once, then prove a reconnect preserves the authoritative score and match state.
for (let i = 0; i < 4; i++) {
  e.input(4, { t: "move", n: 0 });
  if (i < 3) out = e.input(2, { t: "move", n: 1 });
}
out = e.disconnect(4);
out = e.join(5, "ALICE", token);
assert.equal(lastToWs(out, 5, "welcome").msg.resumed, true);
assert.equal(lastToWs(out, 5, "lobby").msg.players.find((p) => p.pid === 1).score, 300);

// At exactly 120 seconds detached, the engine seat expires. The host still knows
// the derived identity, so the same browser credential gets a fresh pid and
// per-game score without asking for the party code again.
e.disconnect(5);
out = e.tick(239998);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
out = e.tick(239999);
assert.ok(out.some((x) => x.to === "uart" && x.kind === "leave" && x.pid === 1));
out = e.join(6, "ALICE", token);
const fresh = lastToWs(out, 6, "welcome").msg;
assert.equal(fresh.resumed, false);
assert.equal(fresh.pid, 1);
const freshStable = out.find((x) => x.to === "uart" && x.kind === "join" && x.pid === 1);
assert.equal(freshStable.identity, identity);
assert.equal(JSON.stringify(out).includes(token), false);

// The compile-time player cap is also enforced at allocation time.
const full = await newEngine();
full.reset();
for (let ws = 1; ws <= 12; ws++) full.join(ws, "P" + ws);
out = full.join(13, "EXTRA");
assert.equal(lastToWs(out, 13, "reject").msg.code, "full");

// Exhausting a game's fixed match table is explicit and does not consume the
// challenge into a phantom match. Pong has four slots at the upstream default.
const cap = await newEngine();
cap.reset();
for (let ws = 1; ws <= 10; ws++) cap.join(ws, "M" + ws);
cap.selectGame(PONG);
for (let ws = 1; ws <= 7; ws += 2) {
  const invite = cap.input(ws, { t: "challenge", to: ws + 1 });
  const id = challengeId(invite, ws + 1);
  cap.input(ws + 1, { t: "accept", id });
}
const overflowInvite = cap.input(9, { t: "challenge", to: 10 });
const overflowId = challengeId(overflowInvite, 10);
out = cap.input(10, { t: "accept", id: overflowId });
assert.equal(lastToWs(out, 9, "error").msg.code, "match_capacity");
assert.equal(lastToWs(out, 10, "error").msg.code, "match_capacity");
assert.equal(challengeId(out, 10), overflowId, "capacity failure leaves the challenge retryable");

// Hello itself enforces the exact grace boundary even if the main loop has not
// ticked since disconnect. At 120,000 ms this is a fresh seat, never stale state.
const boundary = await newEngine();
boundary.resetAt(0);
const boundaryToken = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
boundary.join(1, "BOUNDARY", boundaryToken);
boundary.disconnect(1);
out = boundary.inputAt(2, {
  t: "hello", proto: 2, nick: "BOUNDARY", avatar: "🙂", resume: boundaryToken,
}, 120000);
assert.equal(lastToWs(out, 2, "welcome").msg.resumed, false);

console.log("protocol-v2: all checks passed");
