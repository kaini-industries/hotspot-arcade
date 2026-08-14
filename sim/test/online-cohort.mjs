// Live party lobbies use the online cohort for pack votes and pack selection. A
// detached identity keeps its stored ballot for the 120-second resume grace, but
// that ballot must neither appear in the current tally nor choose the next pack.
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const cases = [
  {
    game: 8, type: "wyr", marker: "ONLINE_A",
    packs: [
      { name: "Online", items: [{ a: "ONLINE_A", b: "ONLINE_B" }] },
      { name: "Offline", items: [{ a: "OFFLINE_A", b: "OFFLINE_B" }] },
    ],
  },
  {
    game: 9, type: "scramble", marker: "AAAAAA",
    packs: [
      { name: "Online", items: [{ word: "AAAAAA" }] },
      { name: "Offline", items: [{ word: "ZZZZZZ" }] },
    ],
  },
  {
    game: 13, type: "spectrum", marker: "ONLINE_LEFT",
    packs: [
      { name: "Online", items: [{ left: "ONLINE_LEFT", right: "ONLINE_RIGHT" }] },
      { name: "Offline", items: [{ left: "OFFLINE_LEFT", right: "OFFLINE_RIGHT" }] },
    ],
  },
  {
    game: 14, type: "kmk", marker: "ONLINE_PERSON_",
    packs: [
      { name: "Online", items: ["A", "B", "C", "D"].map((x) => ({ name: `ONLINE_PERSON_${x}` })) },
      { name: "Offline", items: ["A", "B", "C", "D"].map((x) => ({ name: `OFFLINE_PERSON_${x}` })) },
    ],
  },
  {
    game: 16, type: "secrets", marker: "ONLINE_QUESTION",
    packs: [
      { name: "Online", items: [{ q: "ONLINE_QUESTION" }] },
      { name: "Offline", items: [{ q: "OFFLINE_QUESTION" }] },
    ],
  },
  {
    game: 17, type: "fillblank", marker: "ONLINE _____",
    packs: [
      {
        name: "Online",
        items: [{ p: "ONLINE _____" }, ...Array.from({ length: 20 }, (_, i) => ({ a: `online-${i}` }))],
      },
      {
        name: "Offline",
        items: [{ p: "OFFLINE _____" }, ...Array.from({ length: 20 }, (_, i) => ({ a: `offline-${i}` }))],
      },
    ],
  },
  {
    game: 19, type: "spyfall", marker: "ONLINE_LOCATION",
    packs: [
      { name: "Online", items: ['{"loc":"ONLINE_LOCATION","r":"Online role A","r":"Online role B"}'] },
      { name: "Offline", items: ['{"loc":"OFFLINE_LOCATION","r":"Offline role A","r":"Offline role B"}'] },
    ],
  },
];

for (const c of cases) {
  const e = await newEngine();
  e.reset();
  for (let pid = 1; pid <= 7; pid++) e.join(pid, `P${pid}`);
  e.selectGame(c.game);
  e.loadContent(c.game, c.packs);

  // Three live votes favor pack 0. Four stronger-but-soon-offline ballots favor
  // pack 1; leaving these ballots stored is what makes the regression meaningful.
  for (let pid = 1; pid <= 3; pid++) e.input(pid, { t: "vote", pack: 0 });
  for (let pid = 4; pid <= 7; pid++) e.input(pid, { t: "vote", pack: 1 });
  let lobby = [];
  for (let pid = 4; pid <= 7; pid++) lobby = e.disconnect(pid);

  const lobbyState = lastToWs(lobby, 1, c.type)?.msg;
  assert.ok(lobbyState && lobbyState.phase === "lobby", `${c.type}: lobby survives detach`);
  assert.deepEqual(
    lobbyState.packs.map((p) => p.votes),
    [3, 0],
    `${c.type}: stored offline ballots are excluded from the live tally`,
  );

  for (let pid = 1; pid <= 3; pid++) e.input(pid, { t: "ready", ready: true });
  const started = e.tick(3000);
  const serialized = started
    .filter((x) => x.to === "ws" && x.msg?.t === c.type)
    .map((x) => JSON.stringify(x.msg))
    .join("\n");
  assert.ok(serialized.includes(c.marker), `${c.type}: live voters' pack starts`);
  assert.ok(!serialized.includes("OFFLINE_"), `${c.type}: offline majority cannot choose its pack`);
}

console.log("online cohort: all pack checks passed");
