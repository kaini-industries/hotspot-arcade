// Typed content ingestion smoke tests. Transactions load exactly the selected
// game's bank; malformed input is covered exhaustively in content-bank.mjs.
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const G = { TRIVIA: 1, DRAW: 5, WYR: 8, SCRAMBLE: 9 };

// Trivia maps file keys {q,a,b,c,d,answer} into the engine's typed question.
{
  const e = await newEngine();
  e.reset();
  e.loadContent(G.TRIVIA, [{ name: "General", items: [{
    q: "Capital of France?", a: "Paris", b: "London", c: "Berlin", d: "Madrid", answer: "a",
  }] }]);
  e.join(1, "ANA"); e.join(2, "BO");
  e.input(1, { t: "ready", ready: true });
  e.input(2, { t: "ready", ready: true });
  let out = [];
  for (let ms = 1000; ms <= 8000; ms += 1000) out = out.concat(e.tick(ms));
  const q = lastToWs(out, 1, "trivia").msg;
  assert.deepEqual([...q.o].sort(), ["Berlin", "London", "Madrid", "Paris"].sort());
  const correct = q.o.indexOf("Paris");
  e.input(1, { t: "answer", c: correct });
  out = e.input(2, { t: "answer", c: (correct + 1) % 4 });
  assert.ok(out.some((x) => x.to === "uart" && x.kind === "score" && x.pid === 1));
}

// WYR uses paired a/b strings and preserves multiple packs for an in-game vote.
{
  const e = await newEngine();
  e.reset();
  e.loadContent(G.WYR, [
    { name: "Everyday", items: [{ a: "Tea", b: "Coffee" }] },
    { name: "Spooky", items: [{ a: "Ghosts", b: "Zombies" }] },
  ]);
  e.join(1, "ANA"); e.join(2, "BO");
  e.input(1, { t: "vote", pack: 1 }); e.input(2, { t: "vote", pack: 1 });
  e.input(1, { t: "ready", ready: true }); e.input(2, { t: "ready", ready: true });
  let out = [];
  for (let ms = 1000; ms <= 8000; ms += 1000) out = out.concat(e.tick(ms));
  const state = JSON.stringify(lastToWs(out, 1, "wyr").msg);
  assert.ok(state.includes("Ghosts") && state.includes("Zombies"));
  assert.ok(!state.includes("Tea") && !state.includes("Coffee"));
}

// Word-bank games share storage but retain their game-specific JSON mapping.
{
  const e = await newEngine();
  e.reset();
  e.loadContent(G.SCRAMBLE, [{ name: "Words", items: [{ word: "ELEPHANT" }] }]);
  e.join(1, "ANA"); e.join(2, "BO");
  e.input(1, { t: "ready", ready: true }); e.input(2, { t: "ready", ready: true });
  let out = [];
  for (let ms = 1000; ms <= 8000; ms += 1000) out = out.concat(e.tick(ms));
  const shown = lastToWs(out, 1, "scramble").msg.scram.split("").sort().join("");
  assert.equal(shown, "AEEHLNPT");

  e.loadContent(G.DRAW, [{ name: "Things", items: [{ word: "ROCKET" }] }]);
  out = [];
  for (let ms = 9000; ms <= 11000; ms += 500) out = out.concat(e.tick(ms));
  assert.ok(out.some((x) => x.to === "ws" && x.msg && x.msg.word === "ROCKET"));
}

console.log("content: OK");
