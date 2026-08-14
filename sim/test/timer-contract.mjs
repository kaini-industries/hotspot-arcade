import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { challengeId, lastToWs, newEngine } from "./harness-lib.mjs";

const token = (n) => n.toString(16).padStart(32, "0");
const state = (items, ws, type) => lastToWs(items, ws, type)?.msg;
const LEGACY = new Set(["deadline", "dur", "run", "oms"]);

function noLegacy(value, label, seen = new Set()) {
  if (!value || typeof value !== "object" || seen.has(value)) return;
  seen.add(value);
  for (const [key, child] of Object.entries(value)) {
    assert.equal(LEGACY.has(key), false, `${label}: legacy wire key ${key} is absent`);
    noLegacy(child, label, seen);
  }
}

function timed(msg, label, duration, { exact = true } = {}) {
  assert.ok(msg, `${label}: state exists`);
  noLegacy(msg, label);
  assert.equal(msg.duration_ms, duration, `${label}: duration is milliseconds`);
  assert.equal(Number.isInteger(msg.remaining_ms), true, `${label}: remaining is integer`);
  assert.ok(msg.remaining_ms >= 0 && msg.remaining_ms <= duration, `${label}: remaining is bounded`);
  if (exact) assert.equal(msg.remaining_ms, duration, `${label}: timer starts at full duration`);
  assert.equal(typeof msg.paused, "boolean", `${label}: pause state is explicit`);
  return msg;
}

function untimed(msg, label, { active = true } = {}) {
  assert.ok(msg, `${label}: state exists`);
  noLegacy(msg, label);
  assert.equal(msg.remaining_ms, undefined, `${label}: untimed state omits remaining`);
  assert.equal(msg.duration_ms, undefined, `${label}: untimed state omits duration`);
  if (active) assert.equal(typeof msg.paused, "boolean", `${label}: active state exposes pause`);
  return msg;
}

function secretRed(msg, label) {
  assert.ok(msg);
  noLegacy(msg, label);
  assert.equal(msg.remaining_ms, undefined, `${label}: random red deadline is secret`);
  assert.equal(msg.duration_ms, undefined, `${label}: random red duration is secret`);
  assert.equal(typeof msg.paused, "boolean", `${label}: pause is still explicit`);
}

const source = readFileSync(
  new URL("../../esp32/hotspot-arcade-fw/ha_games.h", import.meta.url), "utf8");
assert.doesNotMatch(source, /\bmillis\s*\(/, "engine time never bypasses logical clocks");
for (const key of LEGACY)
  assert.equal(source.includes(`\\\"${key}\\\"`), false, `wire key ${key} was removed`);

async function joinPlayers(e, count) {
  for (let p = 1; p <= count; p++) e.join(p, `P${p}`, token(p));
}

async function readyAll(e, count) {
  let out = [];
  for (let p = 1; p <= count; p++) out = e.input(p, { t: "ready", ready: true });
  return out;
}

// Trivia: countdown, question, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  let out = await readyAll(e, 2);
  timed(state(out, 1, "trivia"), "Trivia countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "trivia"), "Trivia question", 20000);
  e.input(1, { t: "answer", c: 0 });
  out = e.input(2, { t: "answer", c: 1 });
  timed(state(out, 1, "trivia"), "Trivia reveal", 4000);
}

// Draw: active drawing and reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.loadContent(5, [{ name: "Test", items: [{ word: "clock" }] }]);
  let out = e.tick(0);
  const drawer = [1, 2].find((p) => state(out, p, "draw")?.role === "drawer");
  const guesser = drawer === 1 ? 2 : 1;
  timed(state(out, drawer, "draw"), "Draw round", 70000);
  out = e.input(guesser, { t: "guess", text: "clock" });
  timed(state(out, drawer, "draw"), "Draw reveal", 4000);
}

// Would You Rather: countdown, voting, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.loadContent(8, [{ name: "Test", items: [{ a: "A", b: "B" }] }]);
  let out = await readyAll(e, 2);
  timed(state(out, 1, "wyr"), "WYR countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "wyr"), "WYR vote", 20000);
  e.input(1, { t: "answer", c: 0 });
  out = e.input(2, { t: "answer", c: 1 });
  timed(state(out, 1, "wyr"), "WYR reveal", 5000);
}

// Scramble: countdown, play, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.loadContent(9, [{ name: "Test", items: [{ word: "clock" }] }]);
  let out = await readyAll(e, 2);
  timed(state(out, 1, "scramble"), "Scramble countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "scramble"), "Scramble play", 30000);
  out = e.tick(33000);
  timed(state(out, 1, "scramble"), "Scramble reveal", 5000);
}

// Reaction: countdown, secret random red, public green response, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(7);
  let out = await readyAll(e, 2);
  timed(state(out, 1, "react"), "Reaction countdown", 3000);
  out = e.tick(3000);
  const red = state(out, 1, "react");
  assert.equal(red.light, "wait");
  secretRed(red, "Reaction red");
  out = e.tick(8000); // random red is 2,000..4,999ms, so green is guaranteed
  const green = state(out, 1, "react");
  assert.equal(green.light, "go");
  timed(green, "Reaction green", 6000, { exact: false });
  out = e.input(1, { t: "tap" });
  timed(state(out, 1, "react"), "Reaction reveal", 4000);
}

// Guess Color: countdown, guessing, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(11);
  let out = await readyAll(e, 2);
  timed(state(out, 1, "gc"), "Guess Color countdown", 3000);
  out = e.tick(3000);
  const play = timed(state(out, 1, "gc"), "Guess Color play", 25000);
  const rgb = [1, 3, 5].map((i) => parseInt(play.color.slice(i, i + 2), 16));
  e.input(1, { t: "guess", r: rgb[0], g: rgb[1], b: rgb[2] });
  out = e.input(2, { t: "guess", r: 0, g: 0, b: 0 });
  timed(state(out, 1, "gc"), "Guess Color reveal", 6000);
}

// Spectrum: countdown, psychic clue, group guess, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.loadContent(13, [{ name: "Test", items: [{ left: "Cold", right: "Hot" }] }]);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "spectrum"), "Spectrum countdown", 3000);
  out = e.tick(3000);
  const psychic = [1, 2, 3].find((p) => state(out, p, "spectrum").iam);
  const guessers = [1, 2, 3].filter((p) => p !== psychic);
  timed(state(out, psychic, "spectrum"), "Spectrum clue", 45000);
  out = e.input(psychic, { t: "clue", text: "warm" });
  timed(state(out, guessers[0], "spectrum"), "Spectrum guess", 30000);
  e.input(guessers[0], { t: "slide", n: 50 });
  out = e.input(guessers[1], { t: "slide", n: 60 });
  timed(state(out, psychic, "spectrum"), "Spectrum reveal", 6000);
}

// KMK: countdown, chooser, prediction, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.loadContent(14, [{ name: "Test", items: ["A", "B", "C", "D"].map((name) => ({ name })) }]);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "kmk"), "KMK countdown", 3000);
  out = e.tick(3000);
  const chooser = [1, 2, 3].find((p) => state(out, p, "kmk").iam);
  const guessers = [1, 2, 3].filter((p) => p !== chooser);
  timed(state(out, chooser, "kmk"), "KMK choose", 40000);
  out = e.input(chooser, { t: "assign", kiss: 0, marry: 1, kill: 2 });
  timed(state(out, guessers[0], "kmk"), "KMK guess", 30000);
  e.input(guessers[0], { t: "assign", kiss: 0, marry: 1, kill: 2 });
  out = e.input(guessers[1], { t: "assign", kiss: 1, marry: 0, kill: 2 });
  timed(state(out, chooser, "kmk"), "KMK reveal", 7000);
}

// Secrets: countdown, answer, predict, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.loadContent(16, [{ name: "Test", items: [{ q: "Yes?" }] }]);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "secrets"), "Secrets countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "secrets"), "Secrets answer", 30000);
  e.input(1, { t: "reply", v: 1 });
  e.input(2, { t: "reply", v: 0 });
  out = e.input(3, { t: "reply", v: 1 });
  timed(state(out, 1, "secrets"), "Secrets predict", 30000);
  e.input(1, { t: "predict", n: 2 });
  e.input(2, { t: "predict", n: 2 });
  out = e.input(3, { t: "predict", n: 2 });
  timed(state(out, 1, "secrets"), "Secrets reveal", 5000);
}

// Fillblank: countdown, card play, judging, reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.loadContent(17, [{
    name: "Test",
    items: [{ p: "_____ wins" }, ...Array.from({ length: 30 }, (_, i) => ({ a: `A${i}` }))],
  }]);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "fillblank"), "Fillblank countdown", 3000);
  out = e.tick(3000);
  const czar = [1, 2, 3].find((p) => state(out, p, "fillblank").iam);
  const answerers = [1, 2, 3].filter((p) => p !== czar);
  timed(state(out, czar, "fillblank"), "Fillblank play", 45000);
  e.input(answerers[0], { t: "play", card: 0 });
  out = e.input(answerers[1], { t: "play", card: 0 });
  timed(state(out, czar, "fillblank"), "Fillblank judge", 30000);
  out = e.input(czar, { t: "pick", i: 0 });
  timed(state(out, czar, "fillblank"), "Fillblank reveal", 6000);
}

// Werewolf: countdown and every visible timed role/night/day transition. Five
// players make night one public "no kill", keeping the dynamic day at 160 seconds.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 5);
  e.selectGame(18);
  let out = await readyAll(e, 5);
  timed(state(out, 1, "werewolf"), "Werewolf countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "werewolf"), "Werewolf roles", 12000);
  out = e.tick(15000);
  timed(state(out, 1, "werewolf"), "Werewolf night", 60000);
  out = e.tick(75000);
  timed(state(out, 1, "werewolf"), "Werewolf dawn", 8000);
  out = e.tick(83000);
  timed(state(out, 1, "werewolf"), "Werewolf dynamic day", 160000);
  out = e.tick(243000);
  timed(state(out, 1, "werewolf"), "Werewolf dusk", 8000);
}

// Spyfall: countdown, cards, talk, hush, nomination, poll and reveal.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.loadContent(19, [{
    name: "Test", items: ['{"loc":"Beach","r":"Guard","r":"Surfer","r":"Tourist"}'],
  }]);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "spyfall"), "Spyfall countdown", 3000);
  out = e.tick(3000);
  timed(state(out, 1, "spyfall"), "Spyfall cards", 30000);
  e.input(1, { t: "seen" });
  e.input(2, { t: "seen" });
  out = e.input(3, { t: "seen" });
  timed(state(out, 1, "spyfall"), "Spyfall talk", 360000);
  out = e.tick(363000);
  timed(state(out, 1, "spyfall"), "Spyfall hush", 4000);
  out = e.tick(367000);
  const pick = timed(state(out, 1, "spyfall"), "Spyfall nominate", 30000);
  const nominator = pick.nominator;
  const nominee = [1, 2, 3].find((p) => p !== nominator);
  out = e.input(nominator, { t: "nominate", pid: nominee });
  timed(state(out, 1, "spyfall"), "Spyfall poll", 20000);
  for (const p of [1, 2, 3].filter((x) => x !== nominator))
    out = e.input(p, { t: "agree", in: true });
  timed(state(out, 1, "spyfall"), "Spyfall reveal", 9000);
}

// Frankendraw: countdown, each drawing panel, gallery frames and finale.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 3);
  e.selectGame(20);
  let out = await readyAll(e, 3);
  timed(state(out, 1, "frankendraw"), "Frankendraw countdown", 3000);
  out = e.tick(3000);
  for (let panel = 1; panel <= 3; panel++) {
    timed(state(out, 1, "frankendraw"), `Frankendraw panel ${panel}`, 75000);
    e.input(1, { t: "done" });
    e.input(2, { t: "done" });
    out = e.input(3, { t: "done" });
  }
  timed(state(out, 1, "frankendraw"), "Frankendraw gallery 1", 5000);
  out = e.tick(8000);
  timed(state(out, 1, "frankendraw"), "Frankendraw gallery 2", 5000);
  out = e.tick(13000);
  timed(state(out, 1, "frankendraw"), "Frankendraw gallery 3", 5000);
  out = e.tick(18000);
  const finale = state(out, 1, "frankendraw");
  assert.equal(finale.final, true);
  timed(finale, "Frankendraw finale", 8000);
}

// All seven 1v1 families expose explicit active pause state but no fake timer.
for (const game of [2, 3, 4, 10]) {
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(game);
  const invite = e.input(1, { t: "challenge", to: 2 });
  const out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  untimed(state(out, 1, "duel"), `untimed duel game ${game}`);
}
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(6);
  const invite = e.input(1, { t: "challenge", to: 2 });
  const out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  untimed(state(out, 1, "pong"), "Pong physics state");
}
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(12);
  const invite = e.input(1, { t: "challenge", to: 2 });
  const out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  untimed(state(out, 1, "bs"), "Battleship placement");
}

// Chess uses two relative clocks, and its affected match freezes without changing
// either snapshot while a player is offline.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.selectGame(15);
  const invite = e.input(1, { t: "challenge", to: 2 });
  let out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  let chess = timed(state(out, 1, "chess"), "Chess active clock", 300000);
  assert.equal(chess.other_remaining_ms, 300000);
  e.tick(12345);
  out = e.disconnect(2);
  chess = timed(state(out, 1, "chess"), "Chess paused clock", 300000, { exact: false });
  assert.equal(chess.remaining_ms, 287655);
  assert.equal(chess.other_remaining_ms, 300000);
  assert.equal(chess.paused, true);
  e.tick(72345);
  out = e.join(22, "P2", token(2));
  chess = timed(state(out, 1, "chess"), "Chess resumed clock", 300000, { exact: false });
  assert.equal(chess.remaining_ms, 287655, "paused chess value is frozen exactly");
  assert.equal(chess.other_remaining_ms, 300000);
}

// Planned transport freezes a whole-game visible timer across an arbitrary wall
// clock jump; takeover is used to request a fresh authoritative paused snapshot.
{
  const e = await newEngine();
  e.resetAt(0);
  await joinPlayers(e, 2);
  e.loadContent(8, [{ name: "Test", items: [{ a: "A", b: "B" }] }]);
  await readyAll(e, 2);
  let out = e.tick(3000);
  const before = timed(state(out, 1, "wyr"), "transport freeze before", 20000);
  e.transportPause(2, "", 0);
  e.tick(103000);
  out = e.join(11, "P1", token(1));
  const frozen = timed(state(out, 11, "wyr"), "transport freeze during", 20000);
  assert.equal(frozen.paused, true);
  assert.equal(frozen.remaining_ms, before.remaining_ms);
  out = e.transportResume().out;
  const resumed = timed(state(out, 11, "wyr"), "transport freeze after", 20000);
  assert.equal(resumed.paused, false);
  assert.equal(resumed.remaining_ms, before.remaining_ms);
}

// Rollover-sensitive speed calculations use modular elapsed time rather than raw
// ordering. These scenarios start 16 ms before uint32 wrap and cross it normally.
{
  const e = await newEngine();
  const start = 0xfffffff0;
  e.resetAt(start);
  await joinPlayers(e, 2);
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  await readyAll(e, 2);
  const questionAt = (start + 3000) >>> 0;
  e.tick(questionAt);
  const answerAt = (questionAt + 1000) >>> 0;
  e.inputAt(1, { t: "answer", c: 0 }, answerAt);
  const out = e.inputAt(2, { t: "answer", c: 1 }, answerAt);
  const score = out.find((x) => x.to === "uart" && x.kind === "score" && x.pid === 1);
  assert.equal(score.delta, 975, "Trivia 1s rollover answer keeps its speed bonus");
}
{
  const e = await newEngine();
  const start = 0xfffffff0;
  e.resetAt(start);
  await joinPlayers(e, 2);
  e.selectGame(11);
  await readyAll(e, 2);
  const playAt = (start + 3000) >>> 0;
  let out = e.tick(playAt);
  const play = state(out, 1, "gc");
  const rgb = [1, 3, 5].map((i) => parseInt(play.color.slice(i, i + 2), 16));
  const guessAt = (playAt + 1000) >>> 0;
  e.inputAt(1, { t: "guess", r: rgb[0], g: rgb[1], b: rgb[2] }, guessAt);
  out = e.inputAt(2, { t: "guess", r: 0, g: 0, b: 0 }, guessAt);
  const exact = state(out, 1, "gc").guesses.find((g) => g.pid === 1);
  assert.equal(exact.points, 10, "Guess Color rollover preserves fast exact score");
}
{
  const e = await newEngine();
  const start = 0xfffffff0;
  e.resetAt(start);
  await joinPlayers(e, 2);
  e.selectGame(11);
  await readyAll(e, 2);
  const playAt = (start + 3000) >>> 0;
  const play = state(e.tick(playAt), 1, "gc");
  const rgb = [1, 3, 5].map((i) => parseInt(play.color.slice(i, i + 2), 16));
  const deadline = (playAt + 25000) >>> 0;
  assert.deepEqual(e.inputAt(1, { t: "guess", r: rgb[0], g: rgb[1], b: rgb[2] }, deadline), [],
    "Guess Color rejects input at the exact wrapped deadline");
}

console.log("timer-contract: OK");
