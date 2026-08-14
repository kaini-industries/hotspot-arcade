import assert from "node:assert/strict";
import { challengeId, lastToWs, newEngine } from "./harness-lib.mjs";

const token = (n) => n.toString(16).padStart(32, "0");
const hello = (n, nick = `P${n}`) => ({
  t: "hello", proto: 2, resume: token(n), nick, avatar: "🙂", code: "123456",
});
const wsState = (items, ws, type) => lastToWs(items, ws, type)?.msg;
const uart = (items, kind) => items.filter((x) => x.to === "uart" && x.kind === kind);

async function matched(game, players = [1, 2]) {
  const e = await newEngine();
  e.resetAt(0);
  for (const pid of players) e.join(pid, `P${pid}`, token(pid));
  e.selectGame(game);
  const challenge = e.input(players[0], { t: "challenge", to: players[1] });
  const id = challengeId(challenge, players[1]);
  assert.ok(id, `game ${game}: challenge has an id`);
  e.input(players[1], { t: "accept", id });
  return e;
}

// The four shared-board duel kinds are four independently selected games.  Each
// affected match pauses during grace, ignores mutation, and resumes in place.
for (const game of [2, 3, 4, 10]) {
  const e = await matched(game);
  const detached = e.disconnect(2);
  const frozen = wsState(detached, 1, "duel");
  assert.equal(frozen.paused, true, `game ${game}: affected duel pauses`);
  const before = structuredClone(frozen);
  const n = game === 10 ? frozen.valid?.[0] ?? 19 : 0;
  assert.deepEqual(e.input(1, { t: "move", n }), [], `game ${game}: move is blocked`);
  const resumed = wsState(e.join(22, "P2", token(2)), 1, "duel");
  assert.equal(resumed.paused, false, `game ${game}: reconnect resumes`);
  assert.deepEqual(resumed.board, before.board, `game ${game}: board survives grace`);
  if (game === 4) {
    assert.deepEqual(resumed.hedges, before.hedges, "Dots horizontal edges survive");
    assert.deepEqual(resumed.vedges, before.vedges, "Dots vertical edges survive");
    assert.deepEqual(resumed.boxes, before.boxes, "Dots boxes survive");
  }
}

// Pong is the fifth 1v1 family.  One pairing freezes while another keeps running.
{
  const e = await newEngine();
  e.resetAt(0);
  for (const pid of [1, 2, 3, 4]) e.join(pid, `P${pid}`, token(pid));
  e.selectGame(6);
  let c = e.input(1, { t: "challenge", to: 2 });
  e.input(2, { t: "accept", id: challengeId(c, 2) });
  c = e.input(3, { t: "challenge", to: 4 });
  e.input(4, { t: "accept", id: challengeId(c, 4) });
  e.tick(33);
  const baseline = e.tick(66);
  const start1 = wsState(baseline, 1, "pong");
  const start3 = wsState(baseline, 3, "pong");
  const detached = e.disconnect(2);
  const p1 = wsState(detached, 1, "pong");
  assert.equal(p1.paused, true, "affected Pong match pauses");
  assert.deepEqual(e.input(1, { t: "paddle", dir: 1 }), [], "paused paddle input is ignored");
  const advanced = e.tick(99);
  const p3 = wsState(advanced, 3, "pong");
  assert.deepEqual(p1.ball, start1.ball, "affected Pong physics freezes");
  assert.notDeepEqual(p3.ball, start3.ball, "unrelated Pong match continues");
  const resumed = wsState(e.join(22, "P2", token(2)), 1, "pong");
  assert.equal(resumed.paused, false);
  assert.deepEqual(resumed.ball, p1.ball, "Pong resumes from the frozen ball");
}

// Battleship is the sixth family; placement and play state are both match-local.
{
  const e = await matched(12);
  let out = e.disconnect(2);
  const placement = wsState(out, 1, "bs");
  assert.equal(placement.paused, true);
  assert.deepEqual(
    e.input(1, { t: "place", ships: "0,0,0;1,0,0;2,0,0;3,0,0;4,0,0" }),
    [],
    "placement is blocked during grace",
  );
  e.join(22, "P2", token(2));
  e.input(1, { t: "place", ships: "0,0,0;0,2,0;0,4,0;0,6,0;0,8,0" });
  e.input(22, { t: "place", ships: "0,0,1;2,0,1;4,0,1;6,0,1;8,0,1" });
  out = e.disconnect(22);
  assert.equal(wsState(out, 1, "bs").paused, true, "Battleship play also pauses");
  assert.deepEqual(e.input(1, { t: "fire", x: 0, y: 0 }), [], "fire is blocked");
}

// Chess is the seventh family.  An affected clock freezes exactly while another
// concurrent chess match loses only its own side-to-move time.
{
  const e = await newEngine();
  e.resetAt(0);
  for (const pid of [1, 2, 3, 4]) e.join(pid, `P${pid}`, token(pid));
  e.selectGame(15);
  let c = e.input(1, { t: "challenge", to: 2 });
  e.input(2, { t: "accept", id: challengeId(c, 2) });
  c = e.input(3, { t: "challenge", to: 4 });
  e.input(4, { t: "accept", id: challengeId(c, 4) });
  const detached = e.disconnect(2);
  const frozen = wsState(detached, 1, "chess");
  assert.equal(frozen.paused, true);
  assert.deepEqual(e.input(1, { t: "move", from: 12, to: 28, promo: 0 }), []);
  e.tick(10000);
  const affected = wsState(e.join(22, "P2", token(2)), 1, "chess");
  assert.equal(affected.paused, false);
  assert.equal(affected.remaining_ms, 300000, "offline chess clock is exact");
  const unrelated = wsState(e.join(33, "P3", token(3)), 33, "chess");
  assert.equal(unrelated.paused, false);
  assert.equal(unrelated.remaining_ms, 290000, "unrelated chess match continued for 10s");
  assert.equal(unrelated.other_remaining_ms, 300000);
}

// Every match-family lobby is also a live roster view. A spectator joining after
// selection sees current seats, match occupancy is truthful, and a grace-reserved
// opponent becomes offline without becoming spuriously free.
for (const { game, type } of [
  { game: 2, type: "duel" },
  { game: 6, type: "pong" },
  { game: 12, type: "bs" },
  { game: 15, type: "chess" },
]) {
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "P1", token(1));
  e.join(2, "P2", token(2));
  e.selectGame(game);
  let out = e.join(3, "P3", token(3));
  let lobby = wsState(out, 3, type);
  assert.equal(lobby.phase, "lobby");
  assert.deepEqual(lobby.players.map((p) => p.pid), [1, 2, 3],
    `${type}: late join refreshes selected-game roster`);
  assert.ok(lobby.players.every((p) => p.online && !p.busy));

  const invite = e.input(1, { t: "challenge", to: 2 });
  out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  lobby = wsState(out, 3, type);
  assert.equal(lobby.players.find((p) => p.pid === 1).busy, true);
  assert.equal(lobby.players.find((p) => p.pid === 2).busy, true);
  assert.equal(lobby.players.find((p) => p.pid === 3).busy, false);

  out = e.disconnect(2);
  lobby = wsState(out, 3, type);
  const reserved = lobby.players.find((p) => p.pid === 2);
  assert.equal(reserved.online, false, `${type}: detached opponent is shown offline`);
  assert.equal(reserved.busy, true, `${type}: reserved match seat remains busy during grace`);

  out = e.join(4, "P4", token(4));
  lobby = wsState(out, 4, type);
  const newcomer = lobby.players.find((p) => p.pid === 4);
  assert.deepEqual(newcomer && { online: newcomer.online, busy: newcomer.busy },
    { online: true, busy: false }, `${type}: post-selection newcomer is truthful`);
}

// Trivia's own lobby roster carries presence, and pack voting excludes a reserved
// offline voter immediately rather than leaving a stale ballot in the tally.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "ONLINE", token(1));
  e.join(2, "OFFLINE", token(2));
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  e.input(1, { t: "vote", topic: 0 });
  e.input(2, { t: "vote", topic: 0 });
  const lobby = wsState(e.disconnect(2), 1, "trivia");
  assert.equal(lobby.players.find((p) => p.pid === 1).online, true);
  assert.equal(lobby.players.find((p) => p.pid === 2).online, false);
  assert.equal(lobby.topics[0].votes, 1, "offline Trivia vote is removed from live tally");
}

// Exact transient-grace boundary: hello processing resumes at 119,999 ms, while
// expiry/forfeit wins before hello at 120,000 ms.  Expiry remains exactly-once.
{
  const e = await matched(2);
  e.disconnect(2);
  const out = e.inputAt(22, hello(2), 119999);
  assert.equal(wsState(out, 22, "welcome").resumed, true);
  assert.equal(wsState(out, 1, "duel").paused, false);
  assert.equal(uart(out, "leave").length, 0);
}
{
  const e = await matched(2);
  e.disconnect(2);
  const out = e.inputAt(22, hello(2), 120000);
  assert.equal(wsState(out, 22, "welcome").resumed, false);
  assert.equal(uart(out, "leave").length, 1, "expiry finalized the old seat first");
  assert.equal(uart(out, "score").length, 1, "single match expiry forfeits once");
  const later = e.tick(240000);
  assert.equal(uart(later, "leave").length, 0, "expired seat is not left twice");
  assert.equal(uart(later, "score").length, 0, "forfeit cannot score twice");
}

// If both opponents hit the boundary in one sweep the match is a fair double
// forfeit. Repeated expiry passes cannot manufacture a later winner.
{
  const e = await matched(6);
  e.disconnect(1);
  e.disconnect(2);
  let out = e.tick(120000);
  assert.equal(uart(out, "leave").length, 2);
  assert.equal(uart(out, "score").length, 0, "double expiry awards nobody");
  out = e.tick(120001);
  assert.equal(uart(out, "leave").length, 0);
  assert.equal(uart(out, "score").length, 0, "second sweep remains scoreless");
}

async function startSpectrum() {
  const e = await newEngine();
  e.resetAt(0);
  for (const p of [1, 2, 3]) e.join(p, `P${p}`, token(p));
  e.loadContent(13, [{ name: "Test", items: [{ left: "Cold", right: "Hot" }] }]);
  let out = [];
  for (const p of [1, 2, 3]) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const psychic = [1, 2, 3].find((p) => wsState(out, p, "spectrum").iam);
  return { e, out, psychic };
}

async function startKmk() {
  const e = await newEngine();
  e.resetAt(0);
  for (const p of [1, 2, 3]) e.join(p, `P${p}`, token(p));
  e.loadContent(14, [{ name: "Test", items: ["A", "B", "C", "D"].map((name) => ({ name })) }]);
  let out = [];
  for (const p of [1, 2, 3]) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const chooser = [1, 2, 3].find((p) => wsState(out, p, "kmk").iam);
  return { e, out, chooser };
}

async function startFillblank() {
  const e = await newEngine();
  e.resetAt(0);
  for (const p of [1, 2, 3]) e.join(p, `P${p}`, token(p));
  const items = [
    { p: "_____ wins" },
    ...Array.from({ length: 30 }, (_, i) => ({ a: `answer-${i}` })),
  ];
  e.loadContent(17, [{ name: "Test", items }]);
  let out = [];
  for (const p of [1, 2, 3]) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const czar = [1, 2, 3].find((p) => wsState(out, p, "fillblank").iam);
  return { e, out, czar };
}

// Draw freezes only for the current drawer; an ordinary guesser is noncritical.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "P1", token(1));
  e.join(2, "P2", token(2));
  e.loadContent(5, [{ name: "Things", items: [{ word: "clock" }] }]);
  const out = e.tick(0);
  const drawer = [1, 2].find((p) => wsState(out, p, "draw").role === "drawer");
  const guesser = drawer === 1 ? 2 : 1;
  let state = wsState(e.disconnect(guesser), drawer, "draw");
  assert.equal(state.paused, false, "Draw guesser is noncritical");
  const guesserWs = 20 + guesser;
  e.join(guesserWs, `P${guesser}`, token(guesser));
  state = wsState(e.disconnect(drawer), guesserWs, "draw");
  assert.equal(state.paused, true, "Draw drawer is critical");
  const remaining = state.remaining_ms;
  e.tick(60000);
  state = wsState(e.join(20 + drawer, `P${drawer}`, token(drawer)), guesserWs, "draw");
  assert.equal(state.remaining_ms, remaining, "Draw timer froze exactly");
  assert.equal(state.paused, false);
}

// Spectrum's psychic is critical in both clue and guess, while a guesser is not.
{
  const { e, psychic } = await startSpectrum();
  const guessers = [1, 2, 3].filter((p) => p !== psychic);
  assert.equal(wsState(e.disconnect(guessers[0]), psychic, "spectrum").paused, false);
  const guesserWs = 20 + guessers[0];
  e.join(guesserWs, `P${guessers[0]}`, token(guessers[0]));
  let state = wsState(e.disconnect(psychic), guesserWs, "spectrum");
  assert.equal(state.paused, true, "psychic blocks clue stage");
  const left = state.remaining_ms;
  e.tick(20000);
  const psychicWs = 20 + psychic;
  state = wsState(e.join(psychicWs, `P${psychic}`, token(psychic)), guesserWs, "spectrum");
  assert.equal(state.remaining_ms, left);
  e.input(psychicWs, { t: "clue", text: "warm" });
  state = wsState(e.disconnect(psychicWs), guesserWs, "spectrum");
  assert.equal(state.stage, "guess");
  assert.equal(state.paused, true, "psychic remains critical during guesses");
}

// KMK's chooser likewise blocks both the private choose and public guess stages.
{
  const { e, chooser } = await startKmk();
  const peer = [1, 2, 3].find((p) => p !== chooser);
  let state = wsState(e.disconnect(chooser), peer, "kmk");
  assert.equal(state.stage, "choose");
  assert.equal(state.paused, true);
  const chooserWs = 20 + chooser;
  e.join(chooserWs, `P${chooser}`, token(chooser));
  e.input(chooserWs, { t: "assign", kiss: 0, marry: 1, kill: 2 });
  state = wsState(e.disconnect(chooserWs), peer, "kmk");
  assert.equal(state.stage, "guess");
  assert.equal(state.paused, true, "chooser remains critical during predictions");
}

// Fillblank's Czar blocks both submissions and judging; an answerer does not.
{
  const { e, out, czar } = await startFillblank();
  const answerers = [1, 2, 3].filter((p) => p !== czar);
  assert.equal(wsState(e.disconnect(answerers[0]), czar, "fillblank").paused, false);
  const answererWs = 20 + answerers[0];
  e.join(answererWs, `P${answerers[0]}`, token(answerers[0]));
  let state = wsState(e.disconnect(czar), answererWs, "fillblank");
  assert.equal(state.stage, "play");
  assert.equal(state.paused, true);
  const czarWs = 20 + czar;
  e.join(czarWs, `P${czar}`, token(czar));
  for (const p of answerers) {
    const ws = p === answerers[0] ? answererWs : p;
    const m = wsState(out, p, "fillblank");
    const i = m.hand.findIndex((x) => x);
    e.input(ws, { t: "play", card: i });
  }
  state = wsState(e.disconnect(czarWs), answererWs, "fillblank");
  assert.equal(state.stage, "judge");
  assert.equal(state.paused, true, "Czar remains critical while judging");
}

// Frankendraw pauses only for an unfinished current-panel drawer.
{
  const e = await newEngine();
  e.resetAt(0);
  for (const p of [1, 2, 3]) e.join(p, `P${p}`, token(p));
  e.selectGame(20);
  let out = [];
  for (const p of [1, 2, 3]) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  e.input(2, { t: "done" });
  let state = wsState(e.disconnect(2), 1, "frankendraw");
  assert.equal(state.paused, false, "finished artist is no longer critical");
  e.join(22, "P2", token(2));
  state = wsState(e.disconnect(1), 22, "frankendraw");
  assert.equal(state.paused, true, "unfinished artist blocks the panel");
  const remaining = state.remaining_ms;
  e.tick(60000);
  state = wsState(e.join(21, "P1", token(1)), 22, "frankendraw");
  assert.equal(state.remaining_ms, remaining);
}

// Werewolf deliberately treats every dealt living player as critical in roles and
// night. Testing every seat prevents pause behavior from leaking a hidden role.
{
  const e = await newEngine();
  e.resetAt(0);
  for (let p = 1; p <= 6; p++) e.join(p, `P${p}`, token(p));
  e.selectGame(18);
  let out = [];
  for (let p = 1; p <= 6; p++) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const activeWs = Object.fromEntries(Array.from({ length: 6 }, (_, i) => [i + 1, i + 1]));
  for (let p = 1; p <= 6; p++) {
    const observer = p === 1 ? 2 : 1;
    const state = wsState(e.disconnect(activeWs[p]), activeWs[observer], "werewolf");
    assert.equal(state.stage, "roles");
    assert.equal(state.paused, true, `roles pause for dealt seat ${p}`);
    assert.equal(state.blocker, undefined, "pause exposes no hidden-role blocker");
    e.join(20 + p, `P${p}`, token(p));
    activeWs[p] = 20 + p;
  }
  out = e.tick(15000);
  assert.equal(wsState(out, activeWs[1], "werewolf").stage, "night");
  for (let p = 1; p <= 6; p++) {
    const state = wsState(e.disconnect(activeWs[p]), activeWs[p === 1 ? 2 : 1], "werewolf");
    assert.equal(state.stage, "night");
    assert.equal(state.paused, true, `night pauses for dealt seat ${p}`);
    e.join(40 + p, `P${p}`, token(p));
    activeWs[p] = 40 + p;
  }
}

// Werewolf day is deliberately noncritical: an offline reserved identity keeps its
// role and ballot for grace, but neither counts toward the live public vote until
// that identity reconnects. Offline players remain valid discussion/lynch targets.
{
  const e = await newEngine();
  e.resetAt(0);
  for (let p = 1; p <= 6; p++) e.join(p, `P${p}`, token(p));
  e.selectGame(18);
  for (let p = 1; p <= 6; p++) e.input(p, { t: "ready", ready: true });
  e.tick(3000);  // roles
  e.tick(15000); // quiet night 1
  e.tick(75000); // dawn
  let out = e.tick(83000); // day
  assert.equal(wsState(out, 1, "werewolf").stage, "day");

  out = e.input(1, { t: "accuse", n: 6 });
  assert.equal(wsState(out, 2, "werewolf").votes.some((v) => v.by === 1), true);
  out = e.disconnect(1);
  let day = wsState(out, 2, "werewolf");
  assert.equal(day.paused, false, "day continues during normal grace");
  assert.equal(day.voters, 5, "offline living voter leaves the public denominator");
  assert.equal(day.needed, 3, "strict majority is recomputed from online living voters");
  assert.equal(day.votes.some((v) => v.by === 1), false,
    "offline voter's retained ballot is excluded");
  assert.equal(day.players.find((p) => p.pid === 1).online, false);

  out = e.join(21, "P1", token(1));
  day = wsState(out, 21, "werewolf");
  assert.equal(day.stage, "day");
  assert.equal(day.voters, 6, "grace reconnect restores voter presence");
  assert.equal(day.needed, 4);
  assert.equal(day.myvote, 6, "the reserved ballot returns with its identity");
  assert.equal(day.votes.some((v) => v.by === 1 && v.pid === 6), true);

  e.disconnect(21);
  e.input(2, { t: "accuse", n: 6 });
  e.input(3, { t: "accuse", n: 6 });
  out = e.input(4, { t: "accuse", n: 6 });
  assert.equal(wsState(out, 2, "werewolf").stage, "dusk",
    "online strict majority hammers despite an offline reserved voter");
}

// Spyfall applies the same secrecy-safe rule to every in-round seat throughout
// cards, talk and the nomination sub-stages.
{
  const e = await newEngine();
  e.resetAt(0);
  for (const p of [1, 2, 3]) e.join(p, `P${p}`, token(p));
  e.loadContent(19, [{
    name: "Test", items: ['{"loc":"Beach","r":"Guard","r":"Surfer","r":"Tourist"}'],
  }]);
  let out = [];
  for (const p of [1, 2, 3]) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const activeWs = { 1: 1, 2: 2, 3: 3 };
  for (const p of [1, 2, 3]) {
    const observer = p === 1 ? 2 : 1;
    const state = wsState(e.disconnect(activeWs[p]), activeWs[observer], "spyfall");
    assert.equal(state.stage, "card");
    assert.equal(state.paused, true, `cards pause for in-round seat ${p}`);
    e.join(20 + p, `P${p}`, token(p));
    activeWs[p] = 20 + p;
  }
  for (const p of [1, 2, 3]) e.input(activeWs[p], { t: "seen" });
  for (const p of [1, 2, 3]) {
    const state = wsState(e.disconnect(activeWs[p]), activeWs[p === 1 ? 2 : 1], "spyfall");
    assert.equal(state.stage, "talk");
    assert.equal(state.paused, true, `talk pauses for in-round seat ${p}`);
    e.join(40 + p, `P${p}`, token(p));
    activeWs[p] = 40 + p;
  }
  out = e.tick(363000);
  let state = wsState(out, activeWs[1], "spyfall");
  assert.equal(state.nomStage, "hush");
  assert.equal(wsState(e.disconnect(activeWs[3]), activeWs[1], "spyfall").paused, true);
  e.join(63, "P3", token(3));
  activeWs[3] = 63;
  out = e.tick(367000);
  state = wsState(out, activeWs[1], "spyfall");
  assert.equal(state.nomStage, "pick");
  assert.equal(wsState(e.disconnect(activeWs[2]), activeWs[1], "spyfall").paused, true);
  e.join(62, "P2", token(2));
  activeWs[2] = 62;
  const nominator = wsState(e.join(81, "P1", token(1)), 81, "spyfall").nominator;
  activeWs[1] = 81;
  const nomWs = activeWs[nominator];
  const target = [1, 2, 3].find((p) => p !== nominator);
  out = e.input(nomWs, { t: "nominate", pid: target });
  state = wsState(out, activeWs[1], "spyfall");
  assert.equal(state.nomStage, "poll");
  assert.equal(wsState(e.disconnect(activeWs[3]), activeWs[1], "spyfall").paused, true);
}

// A noncritical offline voter is immediately excluded from online quorum.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "ONLINE", token(1));
  e.join(2, "SLEEPING", token(2));
  e.loadContent(8, [{ name: "Test", items: [{ a: "A", b: "B" }] }]);
  e.input(1, { t: "ready", ready: true });
  e.input(2, { t: "ready", ready: true });
  e.tick(3000);
  e.input(1, { t: "answer", c: 0 });
  const state = wsState(e.disconnect(2), 1, "wyr");
  assert.equal(state.phase, "reveal", "offline noncritical voter cannot block quorum");
  assert.equal(state.paused, false);
}

console.log("disconnect-pause: OK");
