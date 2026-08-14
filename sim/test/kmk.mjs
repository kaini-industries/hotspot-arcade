// Kiss Marry Kill: whole-group party game. Each round a rotating chooser secretly
// assigns Kiss/Marry/Kill to three people drawn from the pack; everyone else predicts
// the chooser's assignment. Points = matching positions (0/1/3, since matching two
// forces the third); the chooser earns the guessers' average. Exercises selectGame +
// ready/vote/assign/again, the choose->guess->reveal flow, and the hidden-info rule
// (a guesser never sees the chooser's assignment before the reveal). Drives the real
// engine headless.
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const KMK = 14;
const e = await newEngine();
e.reset();
e.join(1, "ALICE"); e.join(2, "BOB"); e.join(3, "CARA");
e.selectGame(KMK);

// A pack of >=3 names is required (three people are drawn per round).
e.contentClear();
e.contentPack(KMK, "Test");
for (const n of ["Cleopatra", "Darth Vader", "Taylor Swift", "Sherlock Holmes"])
  e.contentItem(JSON.stringify({ name: n }));

// lobby -> all ready -> countdown -> play(choose)
e.input(1, { t: "ready", ready: true });
e.input(2, { t: "ready", ready: true });
let out = e.input(3, { t: "ready", ready: true });
for (let ms = 1000; ms <= 4000; ms += 1000) out = out.concat(e.tick(ms));

// Find the chooser (iam:true) and the two guessers.
let chooser = 0;
for (const pid of [1, 2, 3]) {
  const m = lastToWs(out, pid, "kmk");
  assert.equal(m.msg.phase, "play", "in play after the countdown");
  assert.equal(m.msg.stage, "choose", "choose stage first");
  assert.ok(Array.isArray(m.msg.people) && m.msg.people.length === 3, "three people drawn");
  if (m.msg.iam) chooser = pid;
}
assert.ok(chooser >= 1, "one player is the chooser");
const guessers = [1, 2, 3].filter((p) => p !== chooser);
// Hidden info: a guesser must NOT see the chooser's assignment during the choose stage.
for (const g of guessers) {
  const m = lastToWs(out, g, "kmk");
  assert.equal(m.msg.answer, undefined, "guessers can't see the assignment before the reveal");
}

// Chooser assigns Kiss=person0, Marry=person1, Kill=person2 -> stage flips to guess.
out = e.input(chooser, { t: "assign", kiss: 0, marry: 1, kill: 2 });
for (const g of guessers) {
  const m = lastToWs(out, g, "kmk");
  assert.equal(m.msg.stage, "guess", "guess stage after the chooser decides");
  assert.equal(m.msg.answer, undefined, "still hidden during the guess stage");
}

// One guesser nails it, the other gets one position. Reveal fires when all have guessed.
e.input(guessers[0], { t: "assign", kiss: 0, marry: 1, kill: 2 }); // exact -> 3
out = e.input(guessers[1], { t: "assign", kiss: 1, marry: 0, kill: 2 }); // only Kill matches -> 1
const rev = lastToWs(out, guessers[0], "kmk");
assert.equal(rev.msg.stage, "reveal", "reveal once everyone guessed");
assert.ok(Array.isArray(rev.msg.answer) && rev.msg.answer.length === 3, "reveal exposes the assignment");
assert.ok(Array.isArray(rev.msg.guesses) && rev.msg.guesses.length === 2, "reveal lists both guesses");
const g0 = rev.msg.guesses.find((x) => x.nick === "ALICE" || x.pts === 3) || rev.msg.guesses[0];
const exact = rev.msg.guesses.find((x) => x.pts === 3);
const partial = rev.msg.guesses.find((x) => x.pts === 1);
assert.ok(exact, "the exact guess scores the full 3");
assert.ok(partial, "the one-position guess scores 1");

// Exact-boundary pid reuse during the hidden choose stage cannot transfer the
// chooser marker, answer, or assignment authority to a fresh identity.
{
  const g = await newEngine();
  g.reset();
  for (const pid of [1, 2, 3]) g.join(pid, "P" + pid);
  g.selectGame(KMK);
  g.contentClear(); g.contentPack(KMK, "Test");
  for (const n of ["A", "B", "C", "D"])
    g.contentItem(JSON.stringify({ name: n }));
  for (const pid of [1, 2, 3]) g.input(pid, { t: "ready", ready: true });
  let hidden = [];
  for (let ms = 1000; ms <= 4000; ms += 1000) hidden = hidden.concat(g.tick(ms));
  const oldChooser = [1, 2, 3].find((pid) => lastToWs(hidden, pid, "kmk").msg.iam);
  g.disconnect(oldChooser);
  const reused = g.inputAt(99, {
    t: "hello", proto: 2, nick: "NEW PLAYER", avatar: "🙂",
    resume: "edededededededededededededededed", code: "123456",
  }, 124000);
  assert.equal(lastToWs(reused, 99, "welcome").msg.resumed, false);
  const state = lastToWs(reused, 99, "kmk").msg;
  assert.equal(state.iam, false, "a recycled pid does not inherit chooser authority");
  assert.equal(state.answer, undefined, "a recycled pid does not receive the hidden answer");
  assert.equal(state.stage, "guess", "expiry advances an abandoned choose stage safely");
  assert.deepEqual(g.input(99, { t: "assign", kiss: 0, marry: 1, kill: 2 }), [],
    "the fresh identity cannot assign as the departed chooser");
}

// A grace-reserved offline seat also cannot be selected for a new round-critical role.
const offline = await newEngine();
offline.reset();
offline.join(1, "OFFLINE");
offline.join(2, "ONLINE-A");
offline.join(3, "ONLINE-B");
offline.selectGame(KMK);
offline.contentClear();
offline.contentPack(KMK, "Test");
for (const n of ["A", "B", "C", "D"]) offline.contentItem(JSON.stringify({ name: n }));
offline.disconnect(1);
offline.input(2, { t: "ready", ready: true });
let onlineOut = offline.input(3, { t: "ready", ready: true });
for (let ms = 1000; ms <= 4000; ms += 1000) onlineOut = onlineOut.concat(offline.tick(ms));
const onlineStates = [2, 3].map((pid) => lastToWs(onlineOut, pid, "kmk").msg);
assert.equal(onlineStates.filter((m) => m.iam).length, 1,
  "exactly one online player is chooser");
assert.notEqual(onlineStates[0].chooser, "OFFLINE",
  "reserved offline seat cannot deadlock the round");

// A noncritical guesser who submits and then drops no longer blocks online quorum, but
// the exact submitted assignment remains in the reveal and keeps its award.
{
  const g = await newEngine();
  g.reset();
  for (const pid of [1, 2, 3, 4]) g.join(pid, `P${pid}`);
  g.loadContent(KMK, [{
    name: "Grace",
    items: ["A", "B", "C", "D"].map((name) => ({ name })),
  }]);
  for (const pid of [1, 2, 3, 4]) g.input(pid, { t: "ready", ready: true });
  const begun = g.tick(3000);
  const chooserPid = [1, 2, 3, 4].find((pid) => lastToWs(begun, pid, "kmk").msg.iam);
  const liveGuessers = [1, 2, 3, 4].filter((pid) => pid !== chooserPid);
  g.input(chooserPid, { t: "assign", kiss: 0, marry: 1, kill: 2 });
  const droppedPid = liveGuessers[0];
  g.input(droppedPid, { t: "assign", kiss: 0, marry: 1, kill: 2 });
  g.disconnect(droppedPid);
  g.input(liveGuessers[1], { t: "assign", kiss: 1, marry: 0, kill: 2 });
  const revealed = g.input(liveGuessers[2], { t: "assign", kiss: 2, marry: 1, kill: 0 });
  const state = lastToWs(revealed, chooserPid, "kmk").msg;
  const dropped = state.guesses.find((x) => x.nick === `P${droppedPid}`);
  assert.ok(dropped && dropped.pts === 3,
    "submitted KMK guess survives grace and scores");
  assert.ok(revealed.some((x) => x.to === "uart" && x.kind === "score" && x.pid === droppedPid),
    "offline reserved guesser receives the submitted-work score event");
}

// Replay keeps the committed people pack but resets per-game phone scores when the
// next countdown completes.
{
  const g = await newEngine();
  g.reset();
  for (const pid of [1, 2, 3]) g.join(pid, `R${pid}`);
  g.loadContent(KMK, [{
    name: "Replay", items: ["A", "B", "C", "D"].map((name) => ({ name })),
  }]);
  for (const pid of [1, 2, 3]) g.input(pid, { t: "ready", ready: true });
  let now = 3000;
  let stateOut = g.tick(now);
  for (let round = 1; round <= 6; round++) {
    const chooserPid = [1, 2, 3].find((pid) => lastToWs(stateOut, pid, "kmk").msg.iam);
    g.input(chooserPid, { t: "assign", kiss: 0, marry: 1, kill: 2 });
    let reveal = [];
    for (const pid of [1, 2, 3].filter((p) => p !== chooserPid))
      reveal = g.input(pid, { t: "assign", kiss: 0, marry: 1, kill: 2 });
    assert.equal(lastToWs(reveal, 1, "kmk").msg.stage, "reveal");
    now += 7000;
    stateOut = g.tick(now);
  }
  assert.equal(lastToWs(stateOut, 1, "kmk").msg.phase, "final");
  g.input(1, { t: "again" });
  g.testSetScore(1, 777);
  for (const pid of [1, 2, 3]) g.input(pid, { t: "ready", ready: true });
  stateOut = g.tick(now + 3000);
  const replay = lastToWs(stateOut, 1, "kmk").msg;
  assert.equal(replay.phase, "play");
  assert.equal(replay.scores.find((p) => p.pid === 1).score, 0,
    "KMK replay starts with a fresh phone scoreboard");
}

console.log("kmk: all checks passed");
