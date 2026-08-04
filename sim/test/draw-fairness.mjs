import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const DRAW = 5;
const e = await newEngine();
e.reset(); e.contentClear();
for (const [pack, word] of [
  ["Animals", "otter"], ["Places", "moon"], ["Things", "clock"],
  ["Actions", "dance"], ["Food", "pizza"], ["People", "pilot"],
]) {
  e.contentPack(DRAW, pack);
  e.contentItem(JSON.stringify({ word }));
}
e.contentCommit();
for (let ws = 1; ws <= 8; ws++) e.join(ws, "P" + ws);
e.selectGame(DRAW);

function roundFrom(items) {
  for (let ws = 1; ws <= 8; ws++) {
    const state = lastToWs(items, ws, "draw");
    if (state?.msg?.phase === "draw" && state.msg.role === "drawer")
      return { drawer: ws, pack: state.msg.pack };
  }
  return null;
}

let now = 0;
let out = e.tick(now);
const rounds = [roundFrom(out)];
assert.deepEqual(rounds[0], { drawer: 1, pack: "Animals" });

// Only the drawer may relay a complete finite 0..1 segment.
let ink = e.input(1, { t: "stroke", x0: 0, y0: 0.25, x1: 1, y1: 0.75 });
assert.equal(ink.filter((x) => x.to === "ws" && x.msg?.t === "ink").length, 7);
for (const bad of [
  '{"t":"stroke","x0":1e999,"y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":0oops,"y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":"0","y0":0,"x1":1,"y1":1}',
  '{"t":"stroke","x0":0,"y0":0,"x1":2,"y1":1}',
  '{"t":"stroke","x0":0,"y0":0,"x1":1}',
]) assert.equal(e.inputRaw(1, bad).length, 0, "invalid/partial stroke is dropped atomically");

for (let r = 1; r < 6; r++) {
  now += 70000; e.tick(now);       // reveal
  now += 4000; out = e.tick(now);  // next round
  rounds.push(roundFrom(out));
}
assert.deepEqual(rounds.map((r) => r.drawer), [1, 2, 3, 4, 5, 6]);
assert.deepEqual(
  rounds.map((r) => r.pack),
  ["Animals", "Places", "Things", "Actions", "Food", "People"],
  "a six-round run visits all six packs exactly once",
);

now += 70000; e.tick(now);
now += 4000; out = e.tick(now);
assert.equal(lastToWs(out, 1, "draw").msg.phase, "final");
e.input(1, { t: "again" });
out = e.tick(now);
rounds.push(roundFrom(out));
now += 70000; e.tick(now);
now += 4000; out = e.tick(now);
rounds.push(roundFrom(out));
assert.deepEqual(rounds.slice(6).map((r) => r.drawer), [7, 8], "replay continues fair drawer rotation");
assert.equal(new Set(rounds.map((r) => r.drawer)).size, 8, "all eight players get a drawing turn");

// Every pack is a shuffled deck: no word repeats until all words have appeared,
// and the cursor survives short two-round replays.
const deck = await newEngine();
deck.reset(); deck.contentClear(); deck.contentPack(DRAW, "Deck");
for (const word of ["alpha", "bravo", "charlie", "delta"])
  deck.contentItem(JSON.stringify({ word }));
deck.contentCommit();
deck.join(1, "A"); deck.join(2, "B"); deck.selectGame(DRAW);
let deckNow = 0;
function drawerWord(items) {
  for (const ws of [1, 2]) {
    const state = lastToWs(items, ws, "draw");
    if (state?.msg?.role === "drawer") return state.msg.word;
  }
  return null;
}
const words = [];
for (let run = 0; run < 2; run++) {
  let state = deck.tick(deckNow);
  words.push(drawerWord(state));
  deckNow += 70000; deck.tick(deckNow);
  deckNow += 4000; state = deck.tick(deckNow);
  words.push(drawerWord(state));
  deckNow += 70000; deck.tick(deckNow);
  deckNow += 4000; deck.tick(deckNow); // final
  if (run === 0) deck.input(1, { t: "again" });
}
assert.equal(new Set(words).size, 4, "a shuffled deck exhausts all words before repeating");

console.log("draw-fairness: all checks passed");
