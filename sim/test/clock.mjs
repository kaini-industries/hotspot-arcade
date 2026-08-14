import assert from "node:assert/strict";
import { newEngine } from "./harness-lib.mjs";

const e = await newEngine();
e.resetAt(0xfffffff0);
assert.equal(e.sessionNow(), 0xfffffff0);
assert.equal(e.gameNow(), 0xfffffff0);
e.tick(0x20);
assert.equal(e.sessionNow(), 0x20, "session clock crosses uint32 rollover");
assert.equal(e.gameNow(), 0x20, "nested game clock crosses uint32 rollover");

e.join(1, "A");
let p = e.transportPause(1, "ARCADE TWO", 600000);
assert.equal(p.result, 0);
assert.equal(e.transportPaused(), true);
assert.equal(e.transportExpected(), 1);
const frozenSession = e.sessionNow();
const frozenGame = e.gameNow();
e.tick(900000);
assert.equal(e.sessionNow(), frozenSession, "planned downtime freezes session time");
assert.equal(e.gameNow(), frozenGame, "planned downtime freezes game time");

p = e.transportPause(1, "ARCADE TWO", 600000);
assert.equal(p.result, 1, "identical pause is idempotent");
assert.equal(e.transportPause(2, "", 0).result, 2, "conflicting pause is explicit");
assert.equal(e.transportPause(1, "", 600001).result, 3, "arguments are bounded");

assert.equal(e.transportResume().result, 0);
assert.equal(e.transportPaused(), false);
assert.equal(e.transportResume().result, 4, "resume while running is explicit");
e.tick(900010);
assert.equal(e.sessionNow(), (frozenSession + 10) >>> 0);
assert.equal(e.gameNow(), (frozenGame + 10) >>> 0);

assert.equal(e.timeReached(0x10, 0xfffffff0), true);
assert.equal(e.timeRemaining(0xfffffff0, 0x20), 48);
console.log("clock: OK");
