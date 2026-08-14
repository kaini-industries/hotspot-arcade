// Phone-initiated game changes are policy requests, not synchronous switches.
// The engine cannot load SD-backed content from a WebSocket callback, so the
// default adapter declines and preserves the exact live game/bank/round.
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const e = await newEngine();
e.reset();
e.loadContent(13, [{ name: "Spectrum", items: [{ left: "Cold", right: "Hot" }] }]);
e.join(1, "P1"); e.join(2, "P2");
e.input(1, { t: "ready", ready: true });

const out = e.input(1, { t: "proposeGame", game: "wyr" });
const result = lastToWs(out, 1, "result");
assert.ok(result, "requester receives a typed policy result");
assert.equal(result.msg.event, "game_change");
assert.equal(result.msg.status, "policy_denied");
assert.equal(result.msg.game, "wyr");
assert.equal(e.contentActiveGame(), 13, "active content bank is unchanged");

const event = out.find((x) => x.to === "uart" && x.kind === "event" && x.json?.gamechange);
assert.equal(event?.json.gamechange, "policy_denied", "host sees the policy result");
assert.equal(lastToWs(out, 2, "gamevote"), undefined, "no empty-pack vote overlay is opened");

// A subsequent ready intent still belongs to the original Spectrum lobby.
const after = e.input(2, { t: "ready", ready: true });
assert.ok(lastToWs(after, 1, "spectrum"), "original game remains authoritative");

console.log("gamevote policy: all checks passed");
