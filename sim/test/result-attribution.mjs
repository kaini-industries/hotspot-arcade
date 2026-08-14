// A detached seat may be finalized and immediately reused by a different identity in
// the same hello call. Finished 1v1 views must retain the original opponent's name and
// must not expose the recycled numeric pid as a stale turn/draw-offer authority.
import assert from "node:assert/strict";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

const CASES = [
  { game: 2, type: "duel", turn: true },
  { game: 6, type: "pong" },
  { game: 12, type: "bs", turn: true },
  { game: 15, type: "chess", turn: true, offer: true },
];

for (const c of CASES) {
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "ORIGINAL");
  e.join(2, "SURVIVOR");
  e.selectGame(c.game);
  const challenged = e.input(1, { t: "challenge", to: 2 });
  e.input(2, { t: "accept", id: challengeId(challenged, 2) });
  e.tick(1000);
  e.disconnect(1);

  const out = e.inputAt(90, {
    t: "hello", proto: 2, nick: "NEWCOMER", avatar: "🙂",
    resume: "92929292929292929292929292929292", code: "123456",
  }, 121000);
  const welcome = lastToWs(out, 90, "welcome");
  assert.equal(welcome.msg.resumed, false, c.type + ": a different identity gets a fresh seat");
  assert.equal(welcome.msg.pid, 1, c.type + ": the expired pid is immediately reusable");

  const survivorPush = lastToWs(out, 2, c.type);
  assert.ok(
    survivorPush,
    c.type + ": expiry pushes the result to the survivor; out=" + JSON.stringify(out),
  );
  const survivor = survivorPush.msg;
  assert.equal(survivor.phase, "over", c.type + ": expiry finalizes the match as a forfeit");
  assert.equal(survivor.opp, "ORIGINAL", c.type + ": result keeps the original opponent name");
  if (c.turn) assert.equal(survivor.turn, 0, c.type + ": no stale turn pid remains");
  if (c.offer) assert.equal(survivor.offer, 0, c.type + ": no stale draw offer remains");

  const newcomer = lastToWs(out, 90, c.type).msg;
  assert.equal(newcomer.phase, "lobby", c.type + ": recycled pid does not inherit the match");
}

console.log("result attribution: all match families preserve immutable identity");
