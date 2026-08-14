// Cross-game switching over the shared runtime union with active-game-only content.
// Every host switch is a transaction: packless games commit zero/zero, content games
// replace the sole live bank before the incoming lobby becomes visible.
import assert from "node:assert/strict";
import { newEngine, lastToWs, challengeId } from "./harness-lib.mjs";

const G = {
  TRIVIA: 1, CONNECT4: 2, TICTACTOE: 3, DOTS: 4, DRAW: 5, PONG: 6, REACT: 7,
  WYR: 8, SCRAMBLE: 9, REVERSI: 10, GUESSCOLOR: 11, BATTLESHIP: 12,
  SPECTRUM: 13, KMK: 14, CHESS: 15, SECRETS: 16, FILLBLANK: 17,
  WEREWOLF: 18, SPYFALL: 19, FRANKENDRAW: 20,
};
const NAME = ["none", "trivia", "connect4", "tictactoe", "dots", "draw", "pong",
  "react", "wyr", "scramble", "reversi", "gc", "bs", "spectrum", "kmk", "chess",
  "secrets", "fillblank", "werewolf", "spyfall", "frankendraw"];

const content = {
  [G.TRIVIA]: [{ name: "Trivia", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }] }],
  [G.DRAW]: [{ name: "Draw", items: [{ word: "ROCKET" }] }],
  [G.WYR]: [{ name: "WYR", items: [{ a: "Tea", b: "Coffee" }] }],
  [G.SCRAMBLE]: [{ name: "Words", items: [{ word: "PLANET" }] }],
  [G.SPECTRUM]: [{ name: "Scale", items: [{ left: "Cold", right: "Hot" }] }],
  [G.KMK]: [{ name: "People", items: [{ name: "Ana" }, { name: "Bo" }, { name: "Cy" }] }],
  [G.SECRETS]: [{ name: "Secrets", items: [{ q: "Secret?" }] }],
  [G.FILLBLANK]: [{ name: "Cards", items: [{ p: "_____ wins." }, { a: "A cat" }] }],
  [G.SPYFALL]: [{ name: "Places", items: ['{"loc":"Beach","r":"Guard","r":"Surfer"}'] }],
};

const e = await newEngine();
e.reset();
for (let i = 1; i <= 4; i++) e.join(i, `P${i}`);

const select = (game, lang = "") => content[game]
  ? e.loadContent(game, content[game], lang)
  : e.selectGame(game);

// Jump among differently sized union members. Each transaction must publish the new
// lobby and leave exactly one bank allocated, never all games' String arrays.
const order = [G.TRIVIA, G.FRANKENDRAW, G.CONNECT4, G.FILLBLANK, G.CHESS, G.SPYFALL,
  G.BATTLESHIP, G.WYR, G.SPECTRUM, G.KMK, G.SECRETS, G.GUESSCOLOR, G.SCRAMBLE,
  G.WEREWOLF, G.DOTS, G.REVERSI, G.TICTACTOE, G.DRAW, G.PONG, G.REACT, G.TRIVIA];
for (const game of order) {
  const out = select(game);
  const lobby = lastToWs(out, 1, "lobby");
  assert.equal(lobby?.msg.game, NAME[game], `lobby follows transaction to ${NAME[game]}`);
  assert.equal(lobby.msg.players.length, 4, "roster survives the runtime swap");
  assert.equal(e.contentBankCount(), 1, "only the active bank is resident");
  assert.equal(e.contentActiveGame(), game, "bank is keyed to the active game");
  for (let pid = 1; pid <= 4; pid++) e.input(pid, { t: "ready", ready: true });
  e.tick(1000);
}

// A live challenge/match is runtime, not identity state, and is cleared by commit.
e.selectGame(G.CONNECT4);
const firstChallenge = e.input(1, { t: "challenge", to: 2 });
e.input(2, { t: "accept", id: challengeId(firstChallenge, 2) });
const switched = e.loadContent(G.TRIVIA, content[G.TRIVIA], "de");
assert.equal(lastToWs(switched, 1, "lobby").msg.game, "trivia");
assert.equal(e.contentActiveLang(), "de");

// Disconnect dispatch after a runtime/content swap must only touch the active
// game's union member. Rejoining during grace must preserve the identity seat.
const disconnected = e.disconnect(3);
assert.ok(lastToWs(disconnected, 1, "lobby"), "disconnect is safe after a runtime/content swap");
e.join(3, "P3");

e.selectGame(G.CONNECT4);
const challenged = e.input(3, { t: "challenge", to: 1 });
assert.ok(lastToWs(challenged, 1, "duel"), "old match slots/challenges were cleared");

assert.ok(e.contentBankMax() <= 2, "one live plus at most one staged bank");
console.log("multigame: all checks passed");
