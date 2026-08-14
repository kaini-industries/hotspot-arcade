import assert from "node:assert/strict";
import fs from "node:fs";

const games = {
  duel: /function renderLobby\(m\)[\s\S]*?A\.players\s*=\s*m\.players\s*\|\|\s*\[\][\s\S]*?lobbyView/,
  pong: /function renderLobby\(m\)[\s\S]*?A\.players\s*=\s*m\.players\s*\|\|\s*\[\][\s\S]*?lobbyView/,
  battleship: /m\.phase === "lobby"[\s\S]*?A\.players\s*=\s*m\.players\s*\|\|\s*\[\][\s\S]*?lobbyView/,
  chess: /m\.phase === "lobby"[\s\S]*?A\.players\s*=\s*m\.players\s*\|\|\s*\[\][\s\S]*?A\.lobbyView/,
};

for (const [name, pattern] of Object.entries(games)) {
  const source = fs.readFileSync(new URL(`../games/${name}.js`, import.meta.url), "utf8");
  assert.match(
    source,
    pattern,
    `${name} must refresh the shared roster before rendering its match lobby`,
  );
}

const app = fs.readFileSync(new URL("../core/app.js", import.meta.url), "utf8");
assert.match(app, /p\.online !== false && !p\.busy/,
  "match lobbies must hide offline and already-matched players");

console.log("web match-lobby roster contract: OK");
