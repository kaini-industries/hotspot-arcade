import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(new URL("../package.json", import.meta.url)));
const sourceFiles = [
  path.join(root, "core", "app.js"),
  ...fs.readdirSync(path.join(root, "games"))
    .filter((name) => name.endsWith(".js"))
    .map((name) => path.join(root, "games", name)),
];
const source = sourceFiles.map((file) => fs.readFileSync(file, "utf8")).join("\n");

for (const [label, pattern] of [
  ["server deadline", /\bm\.deadline\b/],
  ["seconds-valued duration", /\bm\.dur\b/],
  ["legacy chess running clock", /\bm\.run\b/],
  ["legacy chess waiting clock", /\bm\.oms\b/],
  ["wall-clock offset", /\bA\.offset(?:Set)?\b/],
  ["server clock helper", /\bserverNow\b/],
  ["deadline calibration", /\bnoteDeadline\b/],
  ["legacy countdown seconds", /\bm\.secs?\b/],
]) {
  assert.doesNotMatch(source, pattern, `${label} must not appear in browser sources`);
}
assert.doesNotMatch(source, /Date\.now\s*\(/, "elapsed browser timing must use performance.now");
assert.match(source, /performance\.now\s*\(/, "the browser uses a monotonic clock");

const reaction = fs.readFileSync(path.join(root, "games", "react.js"), "utf8");
assert.match(reaction, /if \(go\) A\.timebar/);
assert.match(reaction, /else stopBar\(\)/, "the secret red delay must not render a timer");
const chess = fs.readFileSync(path.join(root, "games", "chess.js"), "utf8");
assert.match(chess, /m\.wtm === m\.white/, "Chess clocks map turn-relative values to the viewer");

const html = fs.readFileSync(path.join(root, "src", "index.html"), "utf8");
assert.match(html, /id="transport"[^>]*aria-modal="true"/);
assert.match(html, /id="game-pause"[\s\S]*Waiting for a player to reconnect/);
assert.doesNotMatch(
  html.match(/id="game-pause"[^>]*>[\s\S]*?<\/div>/)?.[0] || "",
  /\{(?:nick|pid|player)\}/,
  "the role/match pause banner must not identify the missing player",
);

const i18n = fs.readFileSync(path.join(root, "core", "i18n.js"), "utf8");
for (const key of [
  "pause.player", "transport.title", "transport.ssid_change", "transport.ap_off",
  "transport.ssid", "transport.timed", "transport.indefinite",
  "transport.restoring_title", "transport.restoring", "join.server_paused",
]) {
  const count = i18n.split(`"${key}"`).length - 1;
  assert.equal(count, 3, `${key} must exist in en, de, and pt-br`);
}

console.log("web protocol-v22 timer source contract: OK");
