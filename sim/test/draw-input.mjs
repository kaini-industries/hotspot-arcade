import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const DRAW = 5;
const e = await newEngine();
e.reset();
e.contentClear();
e.contentPack(DRAW, "Things");
e.contentItem(JSON.stringify({ word: "clock" }));
e.join(1, "DRAWER");
e.join(2, "GUESSER");
e.selectGame(DRAW);

const started = e.tick(0);
const drawer = [1, 2].find((pid) => lastToWs(started, pid, "draw")?.msg?.role === "drawer");
assert.ok(drawer, "a connected player becomes the drawer");
const guesser = drawer === 1 ? 2 : 1;

const ink = e.input(drawer, { t: "stroke", x0: 0, y0: 0.25, x1: 1, y1: 0.75 });
assert.equal(lastToWs(ink, guesser, "ink").msg.x1, 1, "a complete finite 0..1 stroke is relayed");
assert.deepEqual(e.input(guesser, { t: "stroke", x0: 0, y0: 0, x1: 1, y1: 1 }), [],
  "only the drawer can relay ink");

for (const bad of [
  '{"t":"stroke","x0":1e999,"y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":0oops,"y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":"0","y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":0,"y0":0,"x1":2,"y1":1}',
  '{"t":"stroke","x0":0,"y0":0,"x1":1}',
]) assert.deepEqual(e.inputRaw(drawer, bad), [], "invalid or partial stroke is dropped atomically");

// A hello can expire and immediately reuse a pid without loop() ticking first.
// The fresh identity must not inherit the departed drawer's secret role.
e.disconnect(drawer);
const freshToken = "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0";
const reused = e.inputAt(99, {
  t: "hello", proto: 2, nick: "NEW PLAYER", avatar: "🙂",
  resume: freshToken, code: "123456",
}, 120000);
assert.equal(lastToWs(reused, 99, "welcome").msg.resumed, false);
const freshDraw = lastToWs(reused, 99, "draw").msg;
assert.notEqual(freshDraw.role, "drawer", "a recycled pid does not inherit drawer authority");
assert.equal(freshDraw.phase, "reveal", "expiry ends the abandoned drawing exactly once");

console.log("draw-input: finite/range validation passed");
