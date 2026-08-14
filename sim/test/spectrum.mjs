// Spectrum: wavelength-style party game. One player is the psychic each round: they
// see a hidden 0..100 target between two words and type a clue; everyone else slides
// to guess. Points by closeness; the psychic scores by how well the group guessed.
// Exercises selectGame + ready/vote/clue/slide/again and the clue->guess->reveal flow.
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const SP = 13;
const e = await newEngine();
e.reset();
e.join(1, "ALICE"); e.join(2, "BOB"); e.join(3, "CARA");
e.selectGame(SP);
// A pack is required (spectrumCheckStart no-ops with packCount 0). Load one.
e.contentClear();
e.contentPack(SP, "Test");
e.contentItem(JSON.stringify({ left: "Cold", right: "Hot" }));
e.contentItem(JSON.stringify({ left: "Cheap", right: "Expensive" }));

// lobby -> all ready -> countdown -> play(clue)
e.input(1, { t: "ready", ready: true });
e.input(2, { t: "ready", ready: true });
let out = e.input(3, { t: "ready", ready: true });
for (let ms = 1000; ms <= 4000; ms += 1000) out = out.concat(e.tick(ms));

// Find the psychic (the player whose message has iam:true).
let psychic = 0, target = 0;
for (const pid of [1, 2, 3]) {
  const m = lastToWs(out, pid, "spectrum");
  assert.equal(m.msg.phase, "play", "in play after countdown");
  if (m.msg.iam) { psychic = pid; target = m.msg.target; }
}
assert.ok(psychic >= 1, "one player is the psychic");
assert.ok(target >= 5 && target <= 95, "psychic sees the hidden target (got " + target + ")");
// Non-psychics must NOT see the target during the clue stage (hidden info).
const guessers = [1, 2, 3].filter((p) => p !== psychic);
for (const g of guessers) {
  const m = lastToWs(out, g, "spectrum");
  assert.equal(m.msg.target, undefined, "guessers can't see the target during clue stage");
  assert.equal(m.msg.stage, "clue", "clue stage first");
}

// Psychic types a clue -> stage flips to guess, clue is now visible to all.
out = e.input(psychic, { t: "clue", text: "lukewarm" });
for (const g of guessers) {
  const m = lastToWs(out, g, "spectrum");
  assert.equal(m.msg.stage, "guess", "guess stage after the clue");
  assert.equal(m.msg.clue, "lukewarm", "clue is broadcast to guessers");
}

// One guesser nails the target, the other is far. Reveal fires when all have guessed.
e.input(guessers[0], { t: "slide", n: target });
out = e.input(guessers[1], { t: "slide", n: (target + 60) % 100 });
const rev = lastToWs(out, psychic, "spectrum");
assert.equal(rev.msg.stage, "reveal", "reveal once everyone guessed");
assert.equal(rev.msg.target, target, "reveal exposes the target to everyone");
assert.ok(Array.isArray(rev.msg.guesses) && rev.msg.guesses.length === 2, "reveal lists both guesses");
const near = rev.msg.guesses.find((x) => x.g === target);
assert.ok(near && near.pts >= 4, "an exact guess earns the bullseye (got " + (near && near.pts) + ")");

// Exact-boundary pid reuse during the hidden clue stage cannot transfer the
// psychic marker, target, or clue authority to a fresh identity.
{
  const g = await newEngine();
  g.reset();
  for (const pid of [1, 2, 3]) g.join(pid, "P" + pid);
  g.selectGame(SP);
  g.contentClear(); g.contentPack(SP, "Test");
  g.contentItem(JSON.stringify({ left: "Cold", right: "Hot" }));
  for (const pid of [1, 2, 3]) g.input(pid, { t: "ready", ready: true });
  let hidden = [];
  for (let ms = 1000; ms <= 4000; ms += 1000) hidden = hidden.concat(g.tick(ms));
  const oldPsychic = [1, 2, 3].find((pid) => lastToWs(hidden, pid, "spectrum").msg.iam);
  g.disconnect(oldPsychic);
  const reused = g.inputAt(99, {
    t: "hello", proto: 2, nick: "NEW PLAYER", avatar: "🙂",
    resume: "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", code: "123456",
  }, 124000);
  assert.equal(lastToWs(reused, 99, "welcome").msg.resumed, false);
  const state = lastToWs(reused, 99, "spectrum").msg;
  assert.equal(state.iam, false, "a recycled pid does not inherit psychic authority");
  assert.equal(state.target, undefined, "a recycled pid does not receive the hidden target");
  assert.equal(state.stage, "guess", "expiry advances an abandoned clue stage safely");
  assert.deepEqual(g.input(99, { t: "clue", text: "stolen" }), [],
    "the fresh identity cannot submit the departed psychic's clue");
}

// A detached player keeps a reserved engine seat during grace, but cannot be
// assigned the round-critical psychic role after the online quorum starts.
const offline = await newEngine();
offline.reset();
offline.join(1, "OFFLINE");
offline.join(2, "ONLINE-A");
offline.join(3, "ONLINE-B");
offline.contentClear();
offline.contentPack(SP, "Test");
offline.contentItem(JSON.stringify({ left: "Cold", right: "Hot" }));
offline.selectGame(SP);
offline.disconnect(1);
offline.input(2, { t: "ready", ready: true });
let onlineOut = offline.input(3, { t: "ready", ready: true });
for (let ms = 1000; ms <= 4000; ms += 1000) onlineOut = onlineOut.concat(offline.tick(ms));
const onlineStates = [2, 3].map((pid) => lastToWs(onlineOut, pid, "spectrum").msg);
assert.equal(onlineStates.filter((m) => m.iam).length, 1, "exactly one online player is psychic");
assert.notEqual(onlineStates[0].psychic, "OFFLINE", "reserved offline seat cannot deadlock the round");

// A noncritical guesser who submits and then drops no longer blocks online quorum, but
// their already-valid guess remains in the round result and keeps its award.
{
  const g = await newEngine();
  g.reset();
  for (const pid of [1, 2, 3, 4]) g.join(pid, `P${pid}`);
  g.loadContent(SP, [{ name: "Grace", items: [{ left: "Cold", right: "Hot" }] }]);
  for (const pid of [1, 2, 3, 4]) g.input(pid, { t: "ready", ready: true });
  const begun = g.tick(3000);
  const role = [1, 2, 3, 4].map((pid) => lastToWs(begun, pid, "spectrum").msg);
  const psychicPid = role.findIndex((m) => m.iam) + 1;
  const targetValue = role[psychicPid - 1].target;
  const liveGuessers = [1, 2, 3, 4].filter((pid) => pid !== psychicPid);
  g.input(psychicPid, { t: "clue", text: "grace" });
  const droppedPid = liveGuessers[0];
  g.input(droppedPid, { t: "slide", n: targetValue });
  g.disconnect(droppedPid);
  g.input(liveGuessers[1], { t: "slide", n: 0 });
  const revealed = g.input(liveGuessers[2], { t: "slide", n: 100 });
  const state = lastToWs(revealed, psychicPid, "spectrum").msg;
  const dropped = state.guesses.find((x) => x.nick === `P${droppedPid}`);
  assert.ok(dropped && dropped.g === targetValue && dropped.pts === 4,
    "submitted Spectrum guess survives grace and scores");
  assert.ok(revealed.some((x) => x.to === "uart" && x.kind === "score" && x.pid === droppedPid),
    "offline reserved guesser receives the latched score event");
}

console.log("spectrum: all checks passed");
