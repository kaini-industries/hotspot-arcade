import assert from "node:assert/strict";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

const DRAW = 5;
const e = await newEngine();
e.reset();

assert.equal(e.timeReached(0xfffffff0, 0x20), false);
assert.equal(e.timeRemaining(0xfffffff0, 0x20), 48);
assert.equal(e.timeReached(0x20, 0x20), true);
assert.equal(e.timeRemaining(0x21, 0x20), 0);

// Start immediately before raw millis rolls over. Logical game time begins at
// zero and advances by unsigned deltas, so a portal pause contributes no time.
e.resetAt(0xfffffff0);
e.contentClear();
e.contentPack(DRAW, "Clock");
e.contentItem(JSON.stringify({ word: "watch" }));
e.contentCommit();
e.join(1, "ALICE"); e.join(2, "BOB");
e.selectGame(DRAW);
let out = e.tick(0xfffffff0);
assert.equal(lastToWs(out, 1, "draw").msg.phase, "draw");

e.pause();
e.disconnect(1);
let reconnect = e.join(3, "ALICE", "00000000000000000000000000000001", null);
assert.equal(lastToWs(reconnect, 3, "welcome").msg.resumed, true, "hello remains available while game time is paused");
assert.equal(lastToWs(reconnect, 3, "welcome").msg.paused, true);
assert.equal(lastToWs(reconnect, 3, "server_pause").msg.reconnect_ms, 600000,
  "a reconnect during downtime remains visibly paused");
const waitingNew = e.join(4, "CHARLIE", "00000000000000000000000000000004");
assert.equal(lastToWs(waitingNew, 4, "welcome"), undefined, "new admission waits for host resume");
assert.ok(lastToWs(waitingNew, 4, "server_pause"));
e.tick(0x00010000); // raw clock crossed rollover and advanced while transport was off
const resumedTransport = e.resume();
assert.ok(resumedTransport.some((x) => x.msg?.t === "server_resume"), "resume is explicit to browsers");
out = e.tick(0x0002116f); // 69,999 ms of resumed time
assert.equal(lastToWs(out, 3, "draw"), undefined, "paused time did not consume the round");
out = e.tick(0x00021170); // exactly 70,000 ms
assert.equal(lastToWs(out, 3, "draw").msg.phase, "reveal");

// Resume grace uses the same unsigned-delta rule across raw rollover.
e.resetAt(0xffffff00);
e.join(1, "ALICE"); e.join(2, "BOB");
e.disconnect(1);
out = e.tick((0xffffff00 + 119999) >>> 0);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
out = e.tick((0xffffff00 + 120000) >>> 0);
assert.ok(out.some((x) => x.to === "uart" && x.kind === "leave" && x.pid === 1));

// A duplicate START outside a planned pause is a complete no-op. In particular,
// it cannot turn an ordinary disconnect into an indefinitely renewable seat.
const redundantResume = await newEngine();
redundantResume.resetAt(0);
redundantResume.join(1, "ALICE");
redundantResume.disconnect(1);
redundantResume.tick(119000);
assert.deepEqual(redundantResume.resume(), []);
out = redundantResume.tick(120000);
assert.ok(out.some((x) => x.to === "uart" && x.kind === "leave" && x.pid === 1),
  "redundant resume does not extend transient grace");

// A normal disconnect of a role-critical player freezes only that active round.
// The 120-second identity grace continues on raw host time while the logical
// deadline remains exact.
const critical = await newEngine();
critical.resetAt(1000);
critical.contentClear();
critical.contentPack(DRAW, "Grace");
critical.contentItem(JSON.stringify({ word: "pause" }));
critical.contentCommit();
critical.join(1, "ALICE"); critical.join(2, "BOB");
critical.selectGame(DRAW);
out = critical.tick(1000);
assert.equal(lastToWs(out, 1, "draw").msg.role, "drawer");
out = critical.disconnect(1);
const held = lastToWs(out, 2, "draw").msg;
assert.equal(held.paused, true);
assert.equal(held.remaining_ms, 70000);
assert.deepEqual(critical.input(2, { t: "guess", text: "pause" }), [], "gameplay is blocked during critical grace");
out = critical.tick(120999);
assert.equal(out.some((x) => x.to === "uart" && x.kind === "leave"), false);
out = critical.join(3, "ALICE", "00000000000000000000000000000001");
assert.equal(lastToWs(out, 3, "welcome").msg.resumed, true);
assert.equal(lastToWs(out, 3, "draw").msg.remaining_ms, 70000);
assert.equal(lastToWs(out, 3, "draw").msg.paused, false);
assert.equal(lastToWs(critical.tick(190998), 3, "draw"), undefined);
assert.equal(lastToWs(critical.tick(190999), 3, "draw").msg.phase, "reveal");

// A 1v1 grace freezes that match but does not freeze unrelated engine time.
const pong = await newEngine();
pong.resetAt(0);
pong.join(1, "ALICE"); pong.join(2, "BOB");
pong.selectGame(6);
const invite = pong.input(1, { t: "challenge", to: 2 });
out = pong.input(2, { t: "accept", id: challengeId(invite, 2) });
let before = lastToWs(out, 1, "pong").msg;
assert.deepEqual(pong.input(1, { t: "paddle", dir: 2 }), [], "out-of-range Pong direction is rejected");
out = pong.tick(100);
const afterBadDir = lastToWs(out, 1, "pong").msg;
assert.equal(afterBadDir.p1, before.p1, "invalid direction cannot move the paddle");
before = afterBadDir;
out = pong.disconnect(2);
assert.equal(lastToWs(out, 1, "pong").msg.paused, true);
assert.deepEqual(pong.input(1, { t: "paddle", dir: 1 }), [], "Pong input is blocked while an opponent is offline");
pong.tick(5000);
out = pong.join(3, "BOB", "00000000000000000000000000000002");
const after = lastToWs(out, 1, "pong").msg;
assert.equal(after.paused, false);
assert.deepEqual(after.ball, before.ball, "Pong ball is exact across reconnect grace");

// If the host resumes before a missing critical role returns, the affected
// round remains in its ordinary 120-second grace instead of silently advancing.
const plannedCritical = await newEngine();
plannedCritical.resetAt(0);
plannedCritical.contentClear(); plannedCritical.contentPack(DRAW, "Planned");
plannedCritical.contentItem(JSON.stringify({ word: "still" })); plannedCritical.contentCommit();
plannedCritical.join(1, "ALICE"); plannedCritical.join(2, "BOB");
plannedCritical.selectGame(DRAW); plannedCritical.tick(0);
plannedCritical.pause(); plannedCritical.disconnect(1);
out = plannedCritical.resume();
assert.equal(lastToWs(out, 2, "draw").msg.paused, true, "missing drawer converts to transient grace on resume");
assert.equal(lastToWs(plannedCritical.tick(50000), 2, "draw"), undefined);
out = plannedCritical.join(3, "ALICE", "00000000000000000000000000000001");
assert.equal(lastToWs(out, 3, "draw").msg.remaining_ms, 70000);
assert.equal(lastToWs(out, 3, "draw").msg.paused, false);

// Chess separately owns its clock. Planned downtime plus a missing-opponent
// grace must never be charged to the side whose turn it is.
const chessPause = await newEngine();
chessPause.resetAt(0); chessPause.join(1, "ALICE"); chessPause.join(2, "BOB");
chessPause.selectGame(15);
const chessInvite = chessPause.input(1, { t: "challenge", to: 2 });
chessPause.input(2, { t: "accept", id: challengeId(chessInvite, 2) });
chessPause.tick(10000);
chessPause.pause(); chessPause.disconnect(2);
out = chessPause.resume();
const clockHeld = lastToWs(out, 1, "chess").msg;
assert.equal(clockHeld.paused, true);
chessPause.tick(60000);
out = chessPause.join(3, "BOB", "00000000000000000000000000000002");
const clockReturned = lastToWs(out, 1, "chess").msg;
assert.equal(clockReturned.paused, false);
assert.equal(clockReturned.remaining_ms, clockHeld.remaining_ms, "Chess clock excludes both pause intervals");
assert.equal(clockReturned.other_remaining_ms, clockHeld.other_remaining_ms);

// Planned socket teardown cannot mutate party quorum. One WYR vote is pending;
// disconnecting that voter while globally paused must not auto-reveal the round.
const wyr = await newEngine();
wyr.resetAt(0);
wyr.contentClear(); wyr.contentPack(8, "Pause");
wyr.contentItem(JSON.stringify({ a: "A", b: "B" })); wyr.contentCommit();
wyr.join(1, "ALICE"); wyr.join(2, "BOB"); wyr.selectGame(8);
wyr.input(1, { t: "ready", ready: true }); wyr.input(2, { t: "ready", ready: true });
let wyrSeen = [];
for (let ms = 1000; ms <= 4000; ms += 1000) wyrSeen = wyrSeen.concat(wyr.tick(ms));
assert.equal(lastToWs(wyrSeen, 1, "wyr").msg.phase, "vote");
out = wyr.input(1, { t: "answer", c: 0 });
const beforePause = lastToWs(out, 1, "wyr").msg;
assert.equal(beforePause.phase, "vote");
wyr.pause();
assert.deepEqual(wyr.disconnect(2), [], "planned disconnect emits no game mutation");
out = wyr.join(3, "BOB", "00000000000000000000000000000002");
const duringPause = lastToWs(out, 1, "wyr").msg;
assert.equal(duringPause.phase, "vote", "planned disconnect/reconnect preserves exact party phase");
out = wyr.resume();
assert.equal(lastToWs(out, 1, "wyr").msg.phase, "vote", "resume reconciles once without a false reveal");

// A reboot clears Engine seats but the host ledger survives. Known digests may
// allocate a fresh seat inside the ten-minute paused reconnect window; new code-
// authorized identities above still wait until the host resumes.
const restored = await newEngine();
restored.resetAt(500);
const restoredToken = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
restored.join(1, "RETURNING", restoredToken);
restored.resetKeepKnown(600);
restored.pause();
out = restored.join(2, "RETURNING", restoredToken, null);
assert.equal(lastToWs(out, 2, "welcome").msg.paused, true, "host-known identity reclaims a post-reboot seat");
assert.equal(lastToWs(out, 2, "welcome").msg.resumed, false, "fresh Engine seat is not an exact match resume");

console.log("clock: all checks passed");
