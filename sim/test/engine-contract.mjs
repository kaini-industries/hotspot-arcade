// Cross-cutting engine contracts which must survive feature-stack rebases: one
// bounded typed host-event surface, saturating score awards, ContentBank capacity,
// and Draw's fair persistent decks across "play again" boundaries.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { newEngine, lastToWs } from "./harness-lib.mjs";
import { parseGenericPack, stringifyItem } from "../web/trivia-packs.js";

const uart = (items, kind) => items.filter((x) => x.to === "uart" && x.kind === kind);
const source = (path) => readFileSync(new URL(path, import.meta.url), "utf8");

// All awards use one signed-32-bit saturating path, and the host receives only the
// delta which actually fit. A no-op at the boundary must not create ledger drift.
{
  const e = await newEngine();
  e.reset();
  e.join(1, "BOUNDARY");

  e.testSetScore(1, 2147483640);
  let out = e.testAwardScore(1, 100);
  assert.deepEqual(uart(out, "score").map((x) => x.delta), [7]);
  assert.deepEqual(e.testAwardScore(1, 1), [], "a saturated award emits no phantom delta");
  out = e.join(2, "OBSERVER");
  assert.equal(
    lastToWs(out, 1, "lobby").msg.players.find((p) => p.pid === 1).score,
    2147483647,
  );

  e.testSetScore(1, -2147483640);
  out = e.testAwardScore(1, -100);
  assert.deepEqual(uart(out, "score").map((x) => x.delta), [-8]);
  assert.deepEqual(e.testAwardScore(1, -1), [], "negative saturation is also silent");
  out = e.join(3, "REFRESH");
  assert.equal(
    lastToWs(out, 1, "lobby").msg.players.find((p) => p.pid === 1).score,
    -2147483648,
  );

  const games = source("../../esp32/hotspot-arcade-fw/ha_games.h");
  assert.equal((games.match(/haUartScore\s*\(/g) || []).length, 2,
    "haUartScore appears only as the sink declaration and inside awardScore");
  assert.doesNotMatch(games, /_p\[[^\]]+\]\.score\s*[+\-]=/,
    "game implementations never mutate scores with unchecked arithmetic");
}

// Every active game id reaches the same semantic event sink. Content games are
// selected through real validated banks, while packless games use the same empty
// transaction selected by the production host.
{
  const content = new Map([
    [1, [{ name: "Quiz", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }] }]],
    [5, [{ name: "Draw", items: [{ word: "clock" }] }]],
    [8, [{ name: "WYR", items: [{ a: "left", b: "right" }] }]],
    [9, [{ name: "Words", items: [{ word: "arcade" }] }]],
    [13, [{ name: "Spectrum", items: [{ left: "cold", right: "hot" }] }]],
    [14, [{ name: "People", items: [{ name: "A" }, { name: "B" }, { name: "C" }] }]],
    [16, [{ name: "Secrets", items: [{ q: "A secret?" }] }]],
    [17, [{ name: "Cards", items: [{ p: "_____ wins" }, { a: "arcade" }] }]],
    [19, [{ name: "Places", items: ['{"loc":"Lab","r":"Scientist"}'] }]],
  ]);
  for (let game = 1; game <= 20; game++) {
    const e = await newEngine();
    e.reset();
    e.join(1, `P${game}`);
    if (content.has(game)) e.loadContent(game, content.get(game));
    else e.selectGame(game);
    const events = uart(e.testHostEvent(3, `game-${game}`), "host_event");
    assert.deepEqual(events, [{
      to: "uart", kind: "host_event", event: 3, game, actor: 1, target: 2,
      value: 7, text: `game-${game}`,
    }], `game ${game} uses the typed host-event boundary`);
  }

  const e = await newEngine();
  e.reset(); e.join(1, "UTF8"); e.selectGame(15);
  const event = uart(e.testHostEvent(2, "🙂".repeat(30)), "host_event")[0];
  assert.equal(Buffer.byteLength(event.text, "utf8"), 96, "event text is bounded to 96 bytes");
  assert.equal(event.text, "🙂".repeat(24), "the bound never cuts a UTF-8 code point");

  const lobby = await newEngine();
  lobby.reset(); lobby.join(1, "LOBBY");
  const chat = uart(lobby.input(1, { t: "say", text: "before selection" }), "host_event");
  assert.deepEqual(chat, [{
    to: "uart", kind: "host_event", event: 2, game: 0, actor: 1, target: 0,
    value: 0, text: "before selection",
  }], "party chat remains typed and visible before the first game is selected");

  const espProto = source("../../esp32/hotspot-arcade-fw/ha_proto.h");
  const flipperProto = source("../../flipper/hotspot-arcade/ha_proto.h");
  const flipperSession = source("../../flipper/hotspot-arcade/helpers/ha_session.c");
  assert.match(flipperSession,
    /if\(game\s*>\s*HA_GAME_FRANKENDRAW\s*\|\|\s*actor\s*>\s*HA_MAX_PLAYERS/,
    "the Flipper accepts game 0 lobby events while rejecting ids above the catalog");
  assert.doesNotMatch(flipperSession, /game\s*<\s*HA_GAME_TRIVIA/,
    "lobby CHAT is not discarded by a game-id lower-bound check");
  for (const [name, value] of [
    ["HA_HOST_EVENT_VERSION", 1], ["HA_HOST_EVENT_TEXT_MAX", 96],
    ["HA_HOST_EVENT_HEADER_SIZE", 7], ["HA_HOST_EVT_MATCH_STARTED", 1],
    ["HA_HOST_EVT_CHAT", 2], ["HA_HOST_EVT_ROLE", 3], ["HA_HOST_EVT_ROUND_WIN", 4],
    ["HA_HOST_EVT_ROUND_DRAW", 5], ["HA_HOST_EVT_ROUND_COMPLETE", 6],
    ["HA_HOST_EVT_GAME_FINAL", 7],
  ]) {
    assert.match(espProto, new RegExp(`${name}\\s*(?:=|\\s)\\s*\\(?${value}\\)?`));
    assert.match(flipperProto, new RegExp(`${name}\\s+\\(?${value}\\)?`));
  }
}

// The preserved Spectrum Wild Card pack is complete at the existing per-pack cap;
// capacity grows by pack count (eight), not by inflating every fixed item array.
{
  const wildcard = parseGenericPack(source("../../packs/spectrum/wildcard.txt"), "wildcard");
  assert.equal(wildcard.items.length, 32, "Wild Card retains all 32 left/right pairs");
  const e = await newEngine();
  e.reset();
  assert.equal(e.contentBegin(13, "").ok, true);
  assert.equal(e.contentPack(13, wildcard.name).ok, true);
  for (const item of wildcard.items)
    assert.equal(e.contentItem(stringifyItem(item)).ok, true);
  assert.equal(e.contentCommit(1, 32).ok, true, "a full 32-pair Spectrum pack commits");

  assert.equal(e.contentBegin(13, "").ok, true);
  for (let pack = 0; pack < 8; pack++) {
    assert.equal(e.contentPack(13, `P${pack}`).ok, true);
    assert.equal(e.contentItem(`{"left":"L${pack}","right":"R${pack}"}`).ok, true);
  }
  assert.equal(e.contentCommit(8, 8).ok, true, "eight Spectrum packs commit losslessly");

  assert.equal(e.contentBegin(1, "").ok, true);
  for (let pack = 0; pack < 8; pack++) {
    assert.equal(e.contentPack(1, `T${pack}`).ok, true);
    assert.equal(e.contentItem(
      `{"q":"Q${pack}?","a":"A","b":"B","c":"C","d":"D","answer":"A"}`,
    ).ok, true);
  }
  assert.equal(e.contentCommit(8, 8).ok, true, "eight Trivia topics commit losslessly");
  assert.equal(e.contentBegin(1, "").ok, true);
  for (let pack = 0; pack < 8; pack++) {
    assert.equal(e.contentPack(1, `T${pack}`).ok, true);
    assert.equal(e.contentItem(
      `{"q":"Q${pack}?","a":"A","b":"B","c":"C","d":"D","answer":"A"}`,
    ).ok, true);
  }
  assert.equal(e.contentPack(1, "overflow").ok, false, "a ninth topic is rejected");
  assert.equal(e.contentCommit(8, 8).ok, false, "a poisoned oversized stage cannot publish");
  assert.equal(e.contentActiveGame(), 1, "the prior eight-topic bank remains live");
}

// Draw rotates non-empty packs, shuffles without replacement, blocks a repeat at
// each reshuffle boundary, and preserves both pack/word and drawer cursors on replay.
{
  const e = await newEngine();
  e.reset(); e.join(1, "ONE"); e.join(2, "TWO");
  e.loadContent(5, [0, 1, 2].map((pack) => ({
    name: `Pack ${pack}`,
    items: [{ word: `p${pack}-a` }, { word: `p${pack}-b` }],
  })));
  let now = 0;
  let out = e.tick(now);
  const seen = [[], [], []];
  const drawers = [];
  const packOrder = [];
  for (let round = 0; round < 8; round++) {
    const drawer = [1, 2].find((pid) => lastToWs(out, pid, "draw")?.msg?.role === "drawer");
    assert.ok(drawer, `round ${round + 1} has a drawer`);
    const state = lastToWs(out, drawer, "draw").msg;
    const pack = Number(state.word[1]);
    drawers.push(drawer); packOrder.push(pack); seen[pack].push(state.word);
    const guesser = drawer === 1 ? 2 : 1;
    assert.equal(lastToWs(e.input(guesser, { t: "guess", text: state.word }), drawer, "draw").msg.phase,
      "reveal");
    now += 4000;
    out = e.tick(now);
    if ((round + 1) % 2 === 0 && round < 7) {
      assert.equal(lastToWs(out, 1, "draw").msg.phase, "final");
      e.input(1, { t: "again" });
      out = e.tick(now);
    }
  }
  assert.deepEqual(packOrder, [0, 1, 2, 0, 1, 2, 0, 1],
    "pack round-robin cursor survives replays");
  for (let i = 1; i < drawers.length; i++)
    assert.notEqual(drawers[i], drawers[i - 1], "drawer rotation survives replays");
  for (const words of seen) {
    assert.notEqual(words[0], words[1], "a two-card pack is exhausted without replacement");
    if (words.length > 2)
      assert.notEqual(words[1], words[2], "reshuffle boundary cannot repeat the last card");
  }
}

// A run is capped at six rounds, so the drawer cursor must also survive replay or
// seats seven and eight could be starved forever at a full table.
{
  const e = await newEngine();
  e.reset();
  for (let pid = 1; pid <= 8; pid++) e.join(pid, `P${pid}`);
  e.loadContent(5, [{
    name: "Fairness",
    items: Array.from({ length: 8 }, (_, i) => ({ word: `word-${i}` })),
  }]);
  let now = 0;
  let out = e.tick(now);
  const drawers = [];
  for (let round = 0; round < 8; round++) {
    const drawer = Array.from({ length: 8 }, (_, i) => i + 1)
      .find((pid) => lastToWs(out, pid, "draw")?.msg?.role === "drawer");
    assert.ok(drawer, `full-table round ${round + 1} has a drawer`);
    drawers.push(drawer);
    const word = lastToWs(out, drawer, "draw").msg.word;
    e.input(drawer === 1 ? 2 : 1, { t: "guess", text: word });
    now += 4000;
    out = e.tick(now);
    if (round === 5) {
      assert.equal(lastToWs(out, 1, "draw").msg.phase, "final");
      e.input(1, { t: "again" });
      out = e.tick(now);
    }
  }
  assert.deepEqual(drawers, [1, 2, 3, 4, 5, 6, 7, 8],
    "replay continues drawer rotation so every full-table seat gets a turn");
}

console.log("engine-contract: all checks passed");
