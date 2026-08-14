// Active-game-only transactional ContentBank invariants. This drives the real
// engine through allocation failure, malformed/capped input, score semantics,
// detached resume, and repeated replacement under ASan/UBSan.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { newEngine, lastToWs } from "./harness-lib.mjs";
import { parseGenericPack, resolvePacks, stringifyItem } from "../web/trivia-packs.js";

const G = { TRIVIA: 1, CONNECT4: 2, REACT: 7, WYR: 8, FILLBLANK: 17, SPYFALL: 19 };
const wyr = (label = "A") => [{ name: "Choices", items: [{ a: `${label} left`, b: `${label} right` }] }];

function assertLive(e, game, lang = "") {
  assert.equal(e.contentActiveGame(), game, "failed transaction preserves live game");
  assert.equal(e.contentActiveLang(), lang, "failed transaction preserves live locale");
  assert.equal(e.contentBankCount(), 1, "failed staging bank is reclaimed");
}

// Frankendraw's separate ~28 KiB stroke store may prefer PSRAM, but its internal-heap
// fallback must honor the same adapter reserve gate as a staged ContentBank.
{
  const games = readFileSync(
    new URL("../../esp32/hotspot-arcade-fw/ha_games.h", import.meta.url), "utf8");
  assert.match(games,
    /_fdSheets\s*=\s*\(FdSheet\*\)ps_malloc\(bytes\);[\s\S]*?if\s*\(!_fdSheets\s*&&\s*haContentAllocationAllowed\(\)\)/,
    "Frankendraw cannot consume adapter-reserved internal heap");
}

// Allocation checkpoints at begin, mid-ingest, and commit all preserve the old bank.
{
  const e = await newEngine();
  e.reset();
  e.loadContent(G.WYR, wyr("old"), "de");

  e.contentFailAfter(0);
  assert.equal(e.contentBegin(G.TRIVIA, "pt-br").ok, false, "begin allocation failure is reported");
  assertLive(e, G.WYR, "de");

  e.contentFailAfter(-1);
  assert.equal(e.contentBegin(G.TRIVIA, "pt-br").ok, true);
  e.contentFailAfter(0);
  assert.equal(e.contentPack(G.TRIVIA, "New").ok, false, "mid-transaction String failure is reported");
  assert.equal(e.contentCommit(1, 1).ok, false, "failed stage cannot commit");
  assertLive(e, G.WYR, "de");

  e.contentFailAfter(-1);
  assert.equal(e.contentBegin(G.TRIVIA, "pt-br").ok, true);
  assert.equal(e.contentPack(G.TRIVIA, "Partial").ok, true);
  e.contentFailAfter(1); // question String succeeds; first option allocation is denied
  assert.equal(e.contentItem(
    '{"q":"Question?","a":"A","b":"B","c":"C","d":"D","answer":"A"}',
  ).ok, false, "partially constructed typed items are reclaimed");
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR, "de");

  e.contentFailAfter(-1);
  assert.equal(e.contentBegin(G.WYR, "pt-br").ok, true);
  assert.equal(e.contentPack(G.WYR, "New").ok, true);
  assert.equal(e.contentItem(JSON.stringify({ a: "one", b: "two" })).ok, true);
  e.contentFailAfter(0);
  const failedCommit = e.contentCommit(1, 1);
  assert.equal(failedCommit.ok, false, "commit allocation checkpoint can fail atomically");
  assert.deepEqual(failedCommit.out, [], "failed commit publishes no config or state");
  assertLive(e, G.WYR, "de");
  e.contentFailAfter(-1);
}

// A rejected replacement must preserve an in-progress round, not merely the live
// bank pointer. Continue the old WYR vote after poisoning and aborting a Trivia stage.
{
  const e = await newEngine();
  e.reset();
  e.loadContent(G.WYR, wyr("still-live"), "de");
  e.join(1, "ANA"); e.join(2, "BO");
  e.input(1, { t: "ready", ready: true }); e.input(2, { t: "ready", ready: true });
  let out = [];
  for (let ms = 1000; ms <= 4000; ms += 1000) out = out.concat(e.tick(ms));
  const before = lastToWs(out, 1, "wyr").msg;
  assert.equal(before.phase, "vote");

  assert.equal(e.contentBegin(G.TRIVIA, "pt-br").ok, true);
  assert.equal(e.contentPack(G.TRIVIA, "Broken").ok, true);
  assert.equal(e.contentItem('{"q":"missing options"}').ok, false);
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR, "de");

  out = e.input(1, { t: "answer", c: 0 });
  const after = lastToWs(out, 1, "wyr").msg;
  assert.equal(after.phase, "vote");
  assert.equal(after.round, before.round);
  assert.equal(after.a, before.a);
  assert.equal(after.b, before.b);
  assert.equal(after.myvote, 0, "the preserved round still accepts its next intent");
}

// Malformed/wrong-game/count/cap violations poison only the staged transaction.
{
  assert.throws(
    () => parseGenericPack("Pack: Broken\nthis line has no colon\n"),
    /malformed pack line/,
    "the host parser rejects malformed lines instead of streaming a valid prefix",
  );
  assert.throws(
    () => parseGenericPack("Pack: Broken\nA: valid prefix\n\0malformed tail"),
    /embedded NUL/,
    "the host parser validates bytes after an embedded NUL instead of committing a prefix",
  );

  const e = await newEngine();
  e.reset();
  e.loadContent(G.WYR, wyr("stable"));

  assert.equal(e.contentBegin(255, "").ok, false, "unsupported game is rejected");
  assert.equal(e.contentBegin(G.TRIVIA, "PT_br").ok, false, "unsafe locale is rejected");
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.CONNECT4, "").ok, true);
  assert.equal(e.contentBegin(G.TRIVIA, "PT_br").ok, false);
  assert.equal(
    e.contentCommit(0, 0).ok,
    false,
    "a malformed BEGIN discards an older packless stage instead of committing it",
  );
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.TRIVIA, "").ok, true);
  assert.equal(e.contentPack(G.WYR, "wrong target").ok, false, "pack game must match target");
  assert.equal(e.contentCommit(0, 0).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.TRIVIA, "").ok, true);
  assert.equal(e.contentPack(G.TRIVIA, "Broken").ok, true);
  assert.equal(e.contentItem('{"q":"missing options"}').ok, false, "malformed item is rejected");
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.TRIVIA, "").ok, true);
  assert.equal(e.contentPack(G.TRIVIA, "Bad answer").ok, true);
  assert.equal(e.contentItem(
    '{"q":"Q?","a":"A","b":"B","c":"C","d":"D","answer":"Apple"}',
  ).ok, false, "Trivia answer must be exactly one A-D letter");
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.FILLBLANK, "").ok, true);
  assert.equal(e.contentPack(G.FILLBLANK, "Ambiguous").ok, true);
  assert.equal(
    e.contentItem('{"p":"_____ wins","a":"both kinds"}').ok,
    false,
    "FillBlank records cannot be both prompt and answer cards",
  );
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.WYR, "").ok, true);
  assert.equal(e.contentPack(G.WYR, "Exact").ok, true);
  assert.equal(e.contentItem('{"a":"A","b":"B"}').ok, true);
  assert.equal(e.contentCommit(1, 2).ok, false, "wire counts must match exactly");
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.WYR, "").ok, true);
  assert.equal(e.contentPack(G.WYR, "Cap").ok, true);
  for (let i = 0; i < 32; i++) assert.equal(e.contentItem(`{"a":"A${i}","b":"B${i}"}`).ok, true);
  assert.equal(e.contentItem('{"a":"overflow","b":"overflow"}').ok, false, "common item cap is enforced");
  assert.equal(e.contentCommit(1, 32).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.FILLBLANK, "").ok, true);
  assert.equal(e.contentPack(G.FILLBLANK, "Cards").ok, true);
  for (let i = 0; i < 24; i++) assert.equal(e.contentItem(`{"p":"prompt ${i} _____"}`).ok, true);
  assert.equal(e.contentItem('{"p":"prompt overflow _____"}').ok, false, "FillBlank prompt cap is enforced");
  assert.equal(e.contentCommit(1, 24).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.FILLBLANK, "").ok, true);
  assert.equal(e.contentPack(G.FILLBLANK, "Answers").ok, true);
  assert.equal(e.contentItem('{"p":"_____ wins"}').ok, true);
  for (let i = 0; i < 56; i++)
    assert.equal(e.contentItem(`{"a":"answer ${i}"}`).ok, true);
  assert.equal(e.contentItem('{"a":"answer overflow"}').ok, false,
    "FillBlank answer cap is enforced");
  assert.equal(e.contentCommit(1, 57).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.SPYFALL, "").ok, true);
  assert.equal(e.contentPack(G.SPYFALL, "Places").ok, true);
  assert.equal(
    e.contentItem('{"loc":"Lab","r":"1","r":"2","r":"3","r":"4","r":"5","r":"6","r":"7"}').ok,
    false,
    "Spyfall rejects a seventh role",
  );
  assert.equal(e.contentCommit(1, 0).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.SPYFALL, "").ok, true);
  assert.equal(e.contentPack(G.SPYFALL, "Locations").ok, true);
  for (let i = 0; i < 14; i++)
    assert.equal(e.contentItem(`{"loc":"Location ${i}","r":"Role"}`).ok, true);
  assert.equal(e.contentItem('{"loc":"Location overflow","r":"Role"}').ok, false,
    "Spyfall location cap is enforced");
  assert.equal(e.contentCommit(1, 14).ok, false);
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.TRIVIA, "").ok, true);
  assert.equal(e.contentCommit(0, 0).ok, false, "content games cannot commit an empty lobby");
  assertLive(e, G.WYR);

  assert.equal(e.contentBegin(G.CONNECT4, "").ok, true);
  assert.equal(e.contentCommit(0, 0).ok, true, "packless games require exact zero/zero");
  assertLive(e, G.CONNECT4);
}

// Same-game locale replacement preserves phone score but ends the round in a fresh
// lobby. An actual game change resets the per-game phone scoreboard. A successful
// commit emits exactly one config broadcast and one authoritative game state/player.
{
  const e = await newEngine();
  e.reset();
  const question = (word) => [{ name: "Quiz", items: [{
    q: `${word}?`, a: "RIGHT", b: "NO", c: "NOPE", d: "WRONG", answer: "A",
  }] }];
  e.loadContent(G.TRIVIA, question("old"));
  e.join(1, "ANA"); e.join(2, "BO");
  e.input(1, { t: "ready", ready: true }); e.input(2, { t: "ready", ready: true });
  let out = [];
  for (let ms = 1000; ms <= 8000; ms += 1000) out = out.concat(e.tick(ms));
  const q = lastToWs(out, 1, "trivia").msg;
  const correct = q.o.indexOf("RIGHT");
  e.input(1, { t: "answer", c: correct });
  out = e.input(2, { t: "answer", c: (correct + 1) % 4 });
  const before = lastToWs(out, 1, "lobby").msg.players.find((p) => p.pid === 1).score;
  assert.ok(before > 0, "test player earned a phone score");

  const same = e.loadContent(G.TRIVIA, question("new"), "de");
  assert.equal(same.filter((x) => x.to === "all" && x.msg?.t === "config").length, 1);
  assert.equal(same.filter((x) => x.to === "ws" && x.msg?.t === "lobby").length, 2);
  assert.equal(same.filter((x) => x.to === "ws" && x.msg?.t === "trivia").length, 2);
  assert.equal(lastToWs(same, 1, "trivia").msg.phase, "lobby", "same-game update ends the round");
  assert.equal(
    lastToWs(same, 1, "lobby").msg.players.find((p) => p.pid === 1).score,
    before,
    "same-game locale update preserves phone score",
  );

  const changed = e.selectGame(G.CONNECT4);
  assert.ok(lastToWs(changed, 1, "lobby").msg.players.every((p) => p.score === 0),
    "actual game change resets phone scores");
}

// Detached identity/grace state survives a content swap and resumes just before expiry.
{
  const e = await newEngine();
  e.resetAt(0);
  e.selectGame(G.CONNECT4);
  const token = "0123456789abcdef0123456789abcdef";
  e.join(11, "ANA", token);
  e.disconnect(11);
  e.loadContent(G.WYR, wyr("resume"), "pt-br");
  const resumed = e.inputAt(22, { t: "hello", proto: 2, resume: token, nick: "ANA", avatar: "🙂" }, 119999);
  const welcome = lastToWs(resumed, 22, "welcome").msg;
  assert.equal(welcome.resumed, true);
  assert.equal(welcome.pid, 1, "transaction does not recycle detached pid");
  assert.equal(welcome.lang, "pt-br", "resumed phone gets committed locale");
  assert.equal(lastToWs(resumed, 22, "lobby").msg.players.length, 1, "roster survives transaction");
}

// Repeated replacement must stay at one live plus one staged allocation and free all
// String-owning banks on reset. ASan/LSan covers the 500-cycle lifetime mechanically.
{
  const e = await newEngine();
  e.reset();
  for (let i = 0; i < 500; i++) {
    const game = (i & 1) ? G.WYR : G.REACT;
    assert.equal(e.contentBegin(game, i & 1 ? "de" : "").ok, true);
    assert.ok(e.contentBankCount() <= 2, "at most one live plus one staged bank");
    if (game === G.WYR) {
      assert.equal(e.contentPack(game, "Cycle").ok, true);
      assert.equal(e.contentItem(`{"a":"left ${i}","b":"right ${i}"}`).ok, true);
      assert.equal(e.contentCommit(1, 1).ok, true);
    } else {
      assert.equal(e.contentCommit(0, 0).ok, true);
    }
    assert.equal(e.contentBankCount(), 1);
  }
  assert.ok(e.contentBankMax() <= 2, "allocator observed no third bank");
  e.reset();
  assert.equal(e.contentBankCount(), 0, "reset destroys the live bank");
}

// Exercise every shipped English/localized pack through the same generic parser and
// strict transaction used by the simulated host. This catches a pack that exceeds a
// typed cap or becomes malformed before it can strand real hardware on the old game.
{
  const e = await newEngine();
  for (const lang of ["", "de", "pt-br"]) {
    for (const source of resolvePacks(lang)) {
      e.reset();
      assert.equal(e.contentBegin(source.game, lang).ok, true);
      let itemCount = 0;
      for (const name of source.names) {
        const sub = source.sub ? `${source.sub}/` : "";
        const path = new URL(`../../packs/${source.dir}/${sub}${name}.txt`, import.meta.url);
        const pack = parseGenericPack(readFileSync(path, "utf8"), name);
        assert.equal(e.contentPack(source.game, pack.name).ok, true, path.pathname);
        for (const item of pack.items) {
          assert.equal(e.contentItem(stringifyItem(item)).ok, true, path.pathname);
          itemCount++;
        }
      }
      assert.equal(
        e.contentCommit(source.names.length, itemCount).ok,
        true,
        `${lang || "en"}/${source.dir}`,
      );
    }
  }
}

console.log("content-bank: all checks passed");
