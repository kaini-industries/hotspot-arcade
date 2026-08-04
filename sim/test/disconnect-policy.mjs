// Disconnect policy matrix for every v18 game.
//
// 1v1 games retain an exact match for 120 seconds, reject play while one side is
// offline, resume the same state with the browser token, then forfeit exactly
// once at expiry. Whole-group games remove noncritical offline players from
// quorum. Draw/Spectrum/KMK instead freeze a role-critical round until that role
// returns or its raw-time grace expires.
import assert from "node:assert/strict";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

const GAME = {
  trivia: 1, c4: 2, ttt: 3, dots: 4, draw: 5, pong: 6, react: 7,
  wyr: 8, scramble: 9, reversi: 10, gc: 11, battleship: 12,
  spectrum: 13, kmk: 14, chess: 15,
};
const TOKEN = (pid) => pid.toString(16).padStart(32, "0");
const LAYOUT = "0,0,0;1,0,0;2,0,0;3,0,0;4,0,0";
const engine = await newEngine();

function leaves(items, pid) {
  return items.filter((x) => x.to === "uart" && x.kind === "leave" && x.pid === pid);
}
function wins(items) {
  return items.filter((x) => x.to === "uart" && x.kind === "host_event" && x.event?.type === 4);
}
function assertFinalizedOnce(items, pid, label) {
  assert.equal(leaves(items, pid).length, 1, `${label}: expiry emits one LEAVE`);
  assert.equal(wins(items).length, 1, `${label}: expiry forfeits once`);
}

// The four board duels share one implementation, but exercise every game kind so
// kind-specific board state and legal-input routing cannot bypass the pause guard.
for (const spec of [
  { id: GAME.c4, name: "Connect Four", fields: ["board"], first: 0, second: 1 },
  { id: GAME.ttt, name: "Tic-Tac-Toe", fields: ["board"], first: 0, second: 1 },
  { id: GAME.dots, name: "Dots", fields: ["hedges", "vedges", "boxes"], first: 0, second: 1 },
  { id: GAME.reversi, name: "Reversi", fields: ["board"], first: null, second: null },
]) {
  engine.reset();
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(spec.id);
  const invite = engine.input(1, { t: "challenge", to: 2 });
  let out = engine.input(2, { t: "accept", id: challengeId(invite, 2) });
  const started = lastToWs(out, 1, "duel").msg;
  const first = spec.first ?? started.valid[0];
  out = engine.input(1, { t: "move", n: first });
  const before = lastToWs(out, 2, "duel").msg;
  const second = spec.second ?? before.valid[0];

  out = engine.disconnect(1);
  assert.equal(lastToWs(out, 2, "duel").msg.paused, true, `${spec.name}: opponent grace pauses play`);
  assert.deepEqual(engine.input(2, { t: "move", n: second }), [], `${spec.name}: move blocked during grace`);

  out = engine.join(3, "ALICE", TOKEN(1), null);
  assert.equal(lastToWs(out, 3, "welcome").msg.resumed, true, `${spec.name}: exact seat resumes`);
  const returned = lastToWs(out, 2, "duel").msg;
  assert.equal(returned.paused, false);
  assert.equal(returned.turn, before.turn);
  for (const field of spec.fields)
    assert.deepEqual(returned[field], before[field], `${spec.name}: ${field} survives grace`);

  engine.disconnect(3);
  out = engine.tick(120000);
  assertFinalizedOnce(out, 1, spec.name);
  assert.equal(leaves(engine.tick(120001), 1).length, 0, `${spec.name}: leave is not repeated`);
}

// Pong keeps its physics snapshot while only the affected match is paused.
{
  engine.resetAt(0);
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(GAME.pong);
  const invite = engine.input(1, { t: "challenge", to: 2 });
  engine.input(2, { t: "accept", id: challengeId(invite, 2) });
  const before = lastToWs(engine.tick(100), 2, "pong").msg;
  let out = engine.disconnect(1);
  assert.equal(lastToWs(out, 2, "pong").msg.paused, true);
  engine.tick(1000);
  out = engine.join(3, "ALICE", TOKEN(1), null);
  const returned = lastToWs(out, 2, "pong").msg;
  assert.equal(returned.paused, false);
  assert.deepEqual(returned.ball, before.ball, "Pong ball survives grace");
  assert.equal(returned.p1, before.p1);
  assert.equal(returned.p2, before.p2);
  engine.disconnect(3);
  out = engine.tick(121000);
  assertFinalizedOnce(out, 1, "Pong");
}

// Battleship retains secret placement state and blocks the other player from
// mutating even their own fleet while the opponent is offline.
{
  engine.resetAt(0);
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(GAME.battleship);
  const invite = engine.input(1, { t: "challenge", to: 2 });
  engine.input(2, { t: "accept", id: challengeId(invite, 2) });
  const placed = lastToWs(engine.input(1, { t: "place", ships: LAYOUT }), 1, "bs").msg;
  let out = engine.disconnect(1);
  assert.equal(lastToWs(out, 2, "bs").msg.paused, true);
  assert.deepEqual(engine.input(2, { t: "place", ships: LAYOUT }), [], "Battleship placement is blocked during grace");
  out = engine.join(3, "ALICE", TOKEN(1), null);
  assert.equal(lastToWs(out, 1, "bs"), undefined, "old socket is not rebound");
  assert.equal(lastToWs(out, 3, "bs").msg.ready, placed.ready, "private placement readiness survives grace");
  engine.disconnect(3);
  out = engine.tick(120000);
  assertFinalizedOnce(out, 1, "Battleship");
}

// Chess freezes only this match clock and retains the exact position.
{
  engine.resetAt(0);
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(GAME.chess);
  const invite = engine.input(1, { t: "challenge", to: 2 });
  engine.input(2, { t: "accept", id: challengeId(invite, 2) });
  const before = lastToWs(engine.input(1, { t: "move", from: 12, to: 28 }), 2, "chess").msg;
  let out = engine.disconnect(1);
  const held = lastToWs(out, 2, "chess").msg;
  assert.equal(held.paused, true);
  assert.deepEqual(engine.input(2, { t: "move", from: 52, to: 36 }), [], "Chess move is blocked during grace");
  engine.tick(5000);
  out = engine.join(3, "ALICE", TOKEN(1), null);
  const returned = lastToWs(out, 2, "chess").msg;
  assert.equal(returned.paused, false);
  assert.equal(returned.board, before.board, "Chess position survives grace");
  assert.equal(returned.remaining_ms, held.remaining_ms, "running clock excludes grace");
  engine.disconnect(3);
  out = engine.tick(125000);
  assertFinalizedOnce(out, 1, "Chess");
}

function loadPack(game, name, items) {
  engine.contentClear(); engine.contentPack(game, name);
  for (const item of items) engine.contentItem(JSON.stringify(item));
  engine.contentCommit();
}
function joinPairAndStart(game) {
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(game);
  engine.input(1, { t: "ready", ready: true });
  engine.input(2, { t: "ready", ready: true });
  return engine.tick(3000);
}
function assertQuorumReveal(out, type, label) {
  const state = lastToWs(out, 1, type);
  assert.ok(state, `${label}: disconnect emits authoritative state`);
  assert.equal(state.msg.phase, "reveal", `${label}: offline noncritical player is excluded from quorum`);
  assert.equal(leaves(out, 2).length, 0, `${label}: transient disconnect is not a leave`);
}

// Whole-group noncritical players stop satisfying quorum immediately, but retain
// their seats and per-game state if they return during grace.
{
  engine.resetAt(0);
  loadPack(GAME.trivia, "Trivia", [{ q: "Two?", a: "one", b: "two", c: "three", d: "four", answer: "B" }]);
  joinPairAndStart(GAME.trivia);
  engine.input(1, { t: "answer", c: 1 });
  let out = engine.disconnect(2);
  assertQuorumReveal(out, "trivia", "Trivia");
  out = engine.join(3, "BOB", TOKEN(2), null);
  assert.equal(lastToWs(out, 3, "welcome").msg.resumed, true);
  assert.equal(lastToWs(out, 3, "trivia").msg.phase, "reveal", "Trivia reveal survives reconnect");
}

{
  engine.resetAt(0);
  loadPack(GAME.wyr, "WYR", [{ a: "A", b: "B" }]);
  joinPairAndStart(GAME.wyr);
  engine.input(1, { t: "answer", c: 0 });
  assertQuorumReveal(engine.disconnect(2), "wyr", "Would You Rather");
}

{
  engine.resetAt(0);
  loadPack(GAME.scramble, "Words", [{ word: "pause" }]);
  joinPairAndStart(GAME.scramble);
  engine.input(1, { t: "guess", text: "pause" });
  assertQuorumReveal(engine.disconnect(2), "scramble", "Word Scramble");
}

{
  engine.resetAt(0);
  joinPairAndStart(GAME.react);
  engine.input(1, { t: "tap" }); // deterministic false start: the light is still red at 3s
  assertQuorumReveal(engine.disconnect(2), "react", "Reaction Duel");
}

{
  engine.resetAt(0);
  joinPairAndStart(GAME.gc);
  engine.input(1, { t: "guess", r: 0, g: 0, b: 0 });
  assertQuorumReveal(engine.disconnect(2), "gc", "Guess the Color");
}

async function assertCriticalRole({ game, type, label, load, stageAfterExpiry }) {
  engine.resetAt(0);
  if (load) load();
  engine.join(1, "ALICE"); engine.join(2, "BOB"); engine.selectGame(game);
  let started;
  if (game === GAME.draw) {
    started = engine.tick(0);
  } else {
    engine.input(1, { t: "ready", ready: true });
    engine.input(2, { t: "ready", ready: true });
    started = engine.tick(3000);
  }
  let role = 0;
  for (const pid of [1, 2]) {
    const state = lastToWs(started, pid, type)?.msg;
    if (game === GAME.draw ? state?.role === "drawer" : state?.iam) role = pid;
  }
  assert.ok(role, `${label}: critical role identified`);
  const other = role === 1 ? 2 : 1;
  let out = engine.disconnect(role);
  const held = lastToWs(out, other, type).msg;
  assert.equal(held.paused, true, `${label}: critical role disconnect freezes the round`);
  const remaining = held.remaining_ms;

  const returnWs = 10 + game;
  out = engine.join(returnWs, role === 1 ? "ALICE" : "BOB", TOKEN(role), null);
  const returned = lastToWs(out, other, type).msg;
  assert.equal(returned.paused, false, `${label}: role resume unfreezes the round`);
  assert.equal(returned.remaining_ms, remaining, `${label}: role grace consumes no round time`);

  engine.disconnect(returnWs);
  const expiryAt = game === GAME.draw ? 120000 : 123000;
  out = engine.tick(expiryAt);
  assert.equal(leaves(out, role).length, 1, `${label}: role expires once`);
  const advanced = lastToWs(out, other, type).msg;
  assert.equal(advanced.paused, false, `${label}: expiry releases the critical pause`);
  assert.equal(advanced.stage || advanced.phase, stageAfterExpiry, `${label}: expiry performs game-specific role release`);
  assert.equal(leaves(engine.tick(expiryAt + 1), role).length, 0, `${label}: role leave is not repeated`);
}

await assertCriticalRole({
  game: GAME.draw, type: "draw", label: "Draw",
  load: () => loadPack(GAME.draw, "Draw", [{ word: "clock" }]),
  stageAfterExpiry: "reveal",
});
await assertCriticalRole({
  game: GAME.spectrum, type: "spectrum", label: "Spectrum",
  load: () => loadPack(GAME.spectrum, "Spectrum", [{ left: "Cold", right: "Hot" }]),
  stageAfterExpiry: "guess",
});
await assertCriticalRole({
  game: GAME.kmk, type: "kmk", label: "Kiss Marry Kill",
  load: () => loadPack(GAME.kmk, "KMK", [{ name: "A" }, { name: "B" }, { name: "C" }]),
  stageAfterExpiry: "guess",
});

// A stray duplicate resume call is not a planned transition and must not renew
// the ordinary 120-second grace window.
engine.resetAt(0);
engine.join(1, "ALICE");
engine.disconnect(1);
engine.tick(119999);
engine.resume();
let out = engine.tick(120000);
assert.equal(leaves(out, 1).length, 1, "redundant transportResume cannot extend transient grace");

console.log("disconnect-policy: all 15 games passed");
