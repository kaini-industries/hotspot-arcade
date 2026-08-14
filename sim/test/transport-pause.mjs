import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { challengeId, lastToWs, newEngine } from "./harness-lib.mjs";

const token = (n) => n.toString(16).padStart(32, "0");
const hello = (n, nick = `P${n}`) => ({
  t: "hello", proto: 2, resume: token(n), nick, avatar: "🙂", code: "123456",
});
const ws = (items, id, type) => lastToWs(items, id, type)?.msg;
const uart = (items, kind) => items.filter((x) => x.to === "uart" && x.kind === kind);

// Snapshot, idempotency/conflict, challenge preservation, denial of unknown
// identities, known takeover and exact welcome ordering.
{
  const e = await newEngine();
  e.resetAt(1000);
  e.join(1, "A", token(1));
  e.join(2, "B", token(2));
  e.selectGame(2);
  const invitation = e.input(1, { t: "challenge", to: 2 });
  const invitationId = challengeId(invitation, 2);
  assert.ok(invitationId);

  let step = e.transportPause(1, "NEW ARCADE", 600000);
  assert.equal(step.result, 0);
  assert.equal(e.transportExpected(), 0b11, "online seats are snapshotted exactly");
  assert.equal(e.transportOnlineExpected(), 0b11);
  assert.equal(step.out.at(-1).msg.t, "server_pause");

  step = e.transportPause(1, "NEW ARCADE", 600000);
  assert.equal(step.result, 1, "identical pause is idempotent");
  assert.deepEqual(step.out, [], "idempotent retry does not rebroadcast");
  step = e.transportPause(2, "", 0);
  assert.equal(step.result, 2, "different pause parameters conflict explicitly");
  assert.equal(e.transportExpected(), 0b11, "conflict cannot replace the snapshot");

  const beforeFallback = [
    e.sessionNow(), e.gameNow(), e.transportExpected(), e.transportOnlineExpected(),
  ];
  const fallback = e.transportFallbackSsid("OLD ARCADE");
  assert.equal(fallback.ok, true, "adapter can report a proven fallback SSID");
  assert.deepEqual(fallback.out, [], "fallback metadata replacement is mutation-silent");
  assert.deepEqual(
    [e.sessionNow(), e.gameNow(), e.transportExpected(), e.transportOnlineExpected()],
    beforeFallback,
    "fallback changes no clock epoch or reconnect mask",
  );

  const rejected = e.join(9, "NEW", token(9));
  assert.equal(ws(rejected, 9, "reject").code, "server_paused");
  assert.equal(ws(rejected, 9, "reject").retry_ms, 600000);

  const disconnected = e.disconnect(1);
  assert.equal(uart(disconnected, "leave").length, 0, "planned disconnect defers leave");
  e.tick(601000);
  assert.equal(e.transportOnlineExpected(), 0b10);
  assert.equal(e.sessionNow(), 1000, "ten raw minutes do not age planned grace");

  const resumed = e.join(11, "A", token(1));
  const mine = resumed.filter((x) => x.to === "ws" && x.id === 11).map((x) => x.msg.t);
  assert.deepEqual(mine.slice(0, 5), ["welcome", "config", "server_pause", "lobby", "duel"]);
  assert.equal(ws(resumed, 11, "welcome").resumed, true);
  assert.equal(ws(resumed, 11, "server_pause").ssid, "OLD ARCADE",
    "returning phones receive the actual fallback network name");
  assert.equal(challengeId(resumed, 2), invitationId, "planned disconnect preserves challenge id");
  assert.equal(e.transportOnlineExpected(), 0b11);

  e.disconnect(1); // stale socket from before takeover cannot detach the new socket
  assert.equal(e.transportOnlineExpected(), 0b11);
  const finish = e.transportResume();
  assert.equal(finish.result, 0);
  const all = finish.out.filter((x) => x.to === "all").map((x) => x.msg.t);
  assert.deepEqual(all.slice(0, 2), ["server_resume", "config"]);
  const resumeIndex = finish.out.findIndex((x) => x.msg?.t === "server_resume");
  const configIndex = finish.out.findIndex((x) => x.msg?.t === "config");
  const stateIndex = finish.out.findIndex((x) => x.msg?.t === "duel");
  assert.ok(resumeIndex >= 0 && resumeIndex < configIndex && configIndex < stateIndex,
    "resume -> config -> authoritative state is globally ordered");
  assert.equal(e.transportResume().result, 4, "resume retry is explicitly already-not-paused");
}

// A planned disconnect survives arbitrarily long raw downtime and receives a full
// 120-second transient window only after transport resume.
for (const { after, expected } of [
  { after: 119999, expected: true },
  { after: 120000, expected: false },
]) {
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "A", token(1));
  e.transportPause(2, "", 0);
  e.disconnect(1);
  e.tick(600000);
  assert.equal(e.sessionNow(), 0);
  e.transportResume();
  const out = e.inputAt(10, hello(1, "A"), 600000 + after);
  assert.equal(ws(out, 10, "welcome").resumed, expected,
    `planned grace at ${after} ms has exact boundary semantics`);
  assert.equal(uart(out, "leave").length, expected ? 0 : 1);
}

// Thirty seconds already spent in normal grace survives downtime, leaving exactly
// 90 seconds after resume rather than starting a new grace window.
for (const { after, expected } of [
  { after: 89999, expected: true },
  { after: 90000, expected: false },
]) {
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "A", token(1));
  e.disconnect(1);
  e.tick(30000);
  e.transportPause(2, "", 0);
  e.tick(630000);
  assert.equal(e.sessionNow(), 30000);
  e.transportResume();
  const out = e.inputAt(10, hello(1, "A"), 630000 + after);
  assert.equal(ws(out, 10, "welcome").resumed, expected,
    `preexisting grace at post-resume ${after} ms has exact boundary semantics`);
}

// A paused scheduler tick cannot consume a Pong frame which was already due at
// the pause boundary. The first post-resume tick must advance that exact frame.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "LEFT", token(1));
  e.join(2, "RIGHT", token(2));
  e.selectGame(6);
  const invite = e.input(1, { t: "challenge", to: 2 });
  let out = e.input(2, { t: "accept", id: challengeId(invite, 2) });
  const before = ws(out, 1, "pong").ball;

  e.inputAt(1, { t: "ping" }, 33); // make one 33 ms frame due without ticking it
  e.transportPause(2, "", 0);
  e.tick(600000); // raw downtime must not consume the due frame
  out = e.transportResume().out;
  assert.deepEqual(ws(out, 1, "pong").ball, before, "resume snapshot is exact");
  out = e.tick(600000);
  assert.notDeepEqual(ws(out, 1, "pong").ball, before,
    "first running tick consumes the frame preserved at the pause boundary");
}

// Planned pause may synchronize a live Chess clock to exact zero. Zero is still
// frozen playing state until the host resumes; only the first running tick may flag.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "WHITE", token(1));
  e.join(2, "BLACK", token(2));
  e.selectGame(15);
  const invite = e.input(1, { t: "challenge", to: 2 });
  e.input(2, { t: "accept", id: challengeId(invite, 2) });

  // Advance the raw/session clocks without running chessTick. pauseTransport's
  // chessSyncAll() is therefore the operation that captures exact zero.
  e.inputAt(1, { t: "ping" }, 300000);
  assert.equal(e.transportPause(2, "", 0).result, 0);
  let out = e.join(11, "WHITE", token(1));
  let chess = ws(out, 11, "chess");
  assert.equal(chess.phase, "playing");
  assert.equal(chess.remaining_ms, 0);
  assert.equal(chess.paused, true);
  assert.equal(uart(out, "score").length, 0);

  for (const [raw, takeover] of [[600000, 21], [1200000, 31]]) {
    out = e.tick(raw);
    assert.equal(uart(out, "score").length, 0, "paused zero clock cannot flag");
    out = e.join(takeover, "WHITE", token(1));
    chess = ws(out, takeover, "chess");
    assert.equal(chess.phase, "playing", "repeated paused ticks preserve live match");
    assert.equal(chess.remaining_ms, 0);
    assert.equal(chess.paused, true);
    assert.equal(uart(out, "score").length, 0);
  }

  out = e.transportResume().out;
  chess = ws(out, 31, "chess");
  assert.equal(chess.phase, "playing", "resume itself preserves exact-zero state");
  assert.equal(chess.remaining_ms, 0);
  assert.equal(chess.paused, false);
  assert.equal(uart(out, "score").length, 0);

  out = e.tick(1200000);
  chess = ws(out, 31, "chess");
  assert.equal(chess.phase, "over");
  assert.equal(chess.reason, "flag");
  assert.equal(uart(out, "score").length, 1, "first running tick flags exactly once");
  assert.equal(uart(out, "score")[0].delta, 300);
  out = e.tick(1200001);
  assert.equal(uart(out, "score").length, 0, "finished zero clock cannot flag twice");
}

// Deferred quorum/challenge effects happen once, on explicit transport resume.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "A", token(1));
  e.join(2, "B", token(2));
  e.loadContent(8, [{ name: "Test", items: [{ a: "A", b: "B" }] }]);
  e.input(1, { t: "ready", ready: true });
  e.input(2, { t: "ready", ready: true });
  e.tick(3000);
  e.input(1, { t: "answer", c: 0 });
  e.transportPause(2, "", 0);
  const disconnected = e.disconnect(2);
  assert.equal(disconnected.some((x) => x.msg?.phase === "reveal"), false,
    "planned disconnect does not prematurely change online quorum");
  const first = e.transportResume();
  assert.equal(first.result, 0);
  assert.equal(ws(first.out, 1, "wyr").phase, "reveal",
    "resume applies deferred quorum exactly once");
  const second = e.transportResume();
  assert.equal(second.result, 4);
  assert.equal(second.out.some((x) => x.msg?.phase === "reveal"), false);
}
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "A", token(1));
  e.join(2, "B", token(2));
  e.selectGame(2);
  const invite = e.input(1, { t: "challenge", to: 2 });
  assert.ok(challengeId(invite, 2));
  e.transportPause(2, "", 0);
  e.disconnect(2);
  const first = e.transportResume();
  assert.deepEqual(ws(first.out, 1, "duel").challenges, [],
    "resume removes a challenge involving a still-offline expected player");
  assert.equal(e.transportResume().result, 4);
}

// A partial planned reconnect is presence-only. It cannot start a Trivia countdown
// merely because the sole returning player was ready; explicit resume applies the
// final online quorum once.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "READY", token(1));
  e.join(2, "NOT READY", token(2));
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  e.input(1, { t: "ready", ready: true });
  e.transportPause(2, "", 0);
  e.disconnect(1);
  e.disconnect(2);
  const partial = e.join(11, "READY", token(1));
  assert.equal(ws(partial, 11, "trivia").phase, "lobby",
    "partial reconnect cannot start Trivia while transport is paused");
  const resumed = e.transportResume();
  assert.equal(ws(resumed.out, 11, "trivia").phase, "countdown",
    "explicit resume applies the ready online quorum once");
  assert.equal(e.transportResume().out.length, 0);
}

// Nor can a previously detached, unready identity cancel an existing countdown by
// reconnecting during planned pause. Cancellation is deferred to explicit resume.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "READY 1", token(1));
  e.join(2, "READY 2", token(2));
  e.join(3, "UNREADY", token(3));
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  e.disconnect(3); // reserved but absent before the countdown and transport snapshot
  e.input(1, { t: "ready", ready: true });
  let out = e.input(2, { t: "ready", ready: true });
  assert.equal(ws(out, 1, "trivia").phase, "countdown");
  e.transportPause(2, "", 0);
  e.disconnect(1);
  e.disconnect(2);
  out = e.join(33, "UNREADY", token(3));
  assert.equal(ws(out, 33, "trivia").phase, "countdown",
    "partial reconnect cannot cancel Trivia while transport is paused");
  out = e.transportResume().out;
  assert.equal(ws(out, 33, "trivia").phase, "lobby",
    "explicit resume applies the unready online quorum once");
}

// The same rule protects a live question. A returning player whose answer is the only
// online answer cannot trigger reveal until explicit resume applies deferred quorum.
{
  const e = await newEngine();
  e.resetAt(0);
  e.join(1, "ANSWERED", token(1));
  e.join(2, "WAITING", token(2));
  e.loadContent(1, [{
    name: "Test", items: [{ q: "Q?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  e.input(1, { t: "ready", ready: true });
  e.input(2, { t: "ready", ready: true });
  let out = e.tick(3000);
  const questionRemaining = ws(out, 1, "trivia").remaining_ms;
  e.input(1, { t: "answer", c: 0 });
  e.transportPause(2, "", 0);
  e.disconnect(1);
  e.disconnect(2);
  out = e.join(11, "ANSWERED", token(1));
  const partial = ws(out, 11, "trivia");
  assert.equal(partial.phase, "question",
    "partial reconnect cannot reveal from its sole answer while paused");
  assert.equal(partial.remaining_ms, questionRemaining);
  const resumed = e.transportResume();
  assert.equal(ws(resumed.out, 11, "trivia").phase, "reveal",
    "explicit resume resolves the deferred answer quorum");
  assert.equal(uart(resumed.out, "score").length, 1, "deferred correct answer scores once");
  const repeated = e.transportResume();
  assert.equal(repeated.result, 4);
  assert.equal(uart(repeated.out, "score").length, 0, "deferred reveal cannot score twice");
}

// Hidden/critical roles and their timer remain byte-for-byte stable through planned
// downtime. Werewolf covers every hidden role class without exposing which one blocked.
{
  const e = await newEngine();
  e.resetAt(0);
  for (let p = 1; p <= 6; p++) e.join(p, `P${p}`, token(p));
  e.selectGame(18);
  let out = [];
  for (let p = 1; p <= 6; p++) out = e.input(p, { t: "ready", ready: true });
  out = out.concat(e.tick(3000));
  const before = ws(out, 1, "werewolf");
  assert.equal(before.stage, "roles");
  e.transportPause(1, "AFTER", 600000);
  e.disconnect(1);
  e.tick(603000);
  const joined = e.join(21, "P1", token(1));
  const after = ws(joined, 21, "werewolf");
  assert.equal(after.myrole, before.myrole, "private role survives planned restart");
  assert.equal(after.stage, before.stage);
  assert.equal(after.remaining_ms, before.remaining_ms, "critical timer is unchanged");
  assert.equal(after.paused, true);
  e.transportResume();
}

// Failed ContentBank commit is fully clock-transparent. Successful commit resets the
// selected game to a fresh lobby, emits config before state, and does not lift transport.
{
  const e = await newEngine();
  e.resetAt(500);
  e.join(1, "A", token(1));
  e.join(2, "B", token(2));
  e.loadContent(1, [{
    name: "Old", items: [{ q: "Old?", a: "A", b: "B", c: "C", d: "D", answer: "A" }],
  }]);
  e.input(1, { t: "ready", ready: true });
  e.input(2, { t: "ready", ready: true });
  let out = e.tick(3500);
  const live = ws(out, 1, "trivia");
  assert.equal(live.phase, "question");
  e.transportPause(1, "NEXT", 600000);
  const before = [e.sessionNow(), e.gameNow()];

  e.contentBegin(1, "de");
  e.contentFailAfter(0);
  assert.equal(e.contentPack(1, "broken").ok, false);
  assert.equal(e.contentCommit(1, 0).ok, false);
  assert.deepEqual([e.sessionNow(), e.gameNow()], before);
  assert.equal(e.transportPaused(), true);

  e.contentFailAfter(-1);
  assert.equal(e.contentBegin(1, "de").ok, true);
  assert.equal(e.contentPack(1, "Fresh").ok, true);
  assert.equal(e.contentItem(JSON.stringify({
    q: "Fresh?", a: "A", b: "B", c: "C", d: "D", answer: "B",
  })).ok, true);
  const committed = e.contentCommit(1, 1);
  assert.equal(committed.ok, true);
  assert.equal(e.transportPaused(), true, "successful swap preserves transport pause");
  assert.equal(e.gameNow(), e.sessionNow(), "successful swap starts fresh game time");
  const configIndex = committed.out.findIndex((x) => x.msg?.t === "config");
  const stateIndex = committed.out.findIndex((x) => x.msg?.t === "trivia");
  assert.ok(configIndex >= 0 && configIndex < stateIndex);
  assert.equal(committed.out[configIndex].msg.lang, "de");
  assert.equal(ws(committed.out, 1, "trivia").phase, "lobby");
  assert.equal(ws(committed.out, 1, "trivia").remaining_ms, undefined,
    "old in-progress round was not retained");
}

// Source-level regression for the adapter/host pre-shutdown auto-resume race. These
// checks deliberately pin both sides of the UART contract, while the tiny state model
// below proves the required pause -> pending -> down -> up -> fresh-snapshot sequence.
{
  const ino = readFileSync(
    new URL("../../esp32/hotspot-arcade-fw/hotspot-arcade-fw.ino", import.meta.url), "utf8");
  const session = readFileSync(
    new URL("../../flipper/hotspot-arcade/helpers/ha_session.c", import.meta.url), "utf8");
  const appSource = readFileSync(
    new URL("../../flipper/hotspot-arcade/hotspot_arcade.c", import.meta.url), "utf8");
  const lobbySource = readFileSync(
    new URL(
      "../../flipper/hotspot-arcade/scenes/hotspot_arcade_scene_lobby.c", import.meta.url),
    "utf8",
  );
  const ssidSource = readFileSync(
    new URL(
      "../../flipper/hotspot-arcade/scenes/hotspot_arcade_scene_ssid_input.c",
      import.meta.url,
    ),
    "utf8",
  );
  assert.match(ino, /networkReady\s*=\s*portalRunning\s*&&\s*!networkSuspendPending/,
    "pending shutdown is never advertised network-ready");
  assert.match(ino, /portalRunning\s*\?\s*0x08\s*:\s*0/,
    "transport snapshot distinguishes portal-live from resume-ready");
  const startPortalSource = ino.slice(
    ino.indexOf("static bool startPortal"), ino.indexOf("static void suspendPortalNetwork"),
  );
  assert.match(startPortalSource,
    /if\(portalRunning\)[\s\S]*?ap_already[\s\S]*?up ip=[\s\S]*?uartTransportState/,
    "idempotent START re-emits both up and authoritative state");
  const pingBlock = ino.slice(ino.indexOf("if(now - lastPing >= 2000)"));
  assert.match(pingBlock, /uartSend\(HA_MSG_PING[\s\S]*?uartTransportState\(\)/,
    "two-second beacon repeats state after any single dropped lifecycle frame");
  assert.match(ino,
    /case HA_MSG_TRANSPORT_RESUME[\s\S]*?if\s*\(portalRunning\s*&&\s*!networkSuspendPending\)/,
    "adapter refuses RESUME during the flush window");
  const pauseHandler = ino.slice(
    ino.indexOf("case HA_MSG_TRANSPORT_PAUSE"), ino.indexOf("case HA_MSG_TRANSPORT_RESUME"));
  const pauseCallIndex = pauseHandler.indexOf("result = engine.pauseTransport");
  const pendingIndex = pauseHandler.indexOf("networkSuspendPending = portalRunning", pauseCallIndex);
  const unlockIndex = pauseHandler.indexOf("ENGINE_UNLOCK()", pendingIndex);
  assert.ok(pauseCallIndex >= 0 && pauseCallIndex < pendingIndex && pendingIndex < unlockIndex,
    "successful pause publishes network-not-ready under the engine mutex before unlock");
  assert.match(pauseHandler,
    /if\s*\(result\s*==\s*HA_TRANSPORT_OK\)\s*\{[\s\S]*?networkSuspendPending\s*=\s*portalRunning[\s\S]*?\}\s*ENGINE_UNLOCK/,
    "only a newly accepted PAUSE arms network suspension");
  assert.doesNotMatch(pauseHandler,
    /HA_TRANSPORT_OK\s*\|\|\s*result\s*==\s*HA_TRANSPORT_ALREADY[\s\S]*?networkSuspendPending/,
    "an idempotent duplicate cannot re-suspend a recovered AP");
  assert.match(session,
    /strncmp\(tok, "up", 2\)[\s\S]*?transport_network_ready\s*=\s*false/,
    "STATUS up invalidates stale network readiness");
  const statusHandler = session.slice(
    session.indexOf("static void on_status"), session.indexOf("// ---------------- frame dispatch"),
  );
  assert.match(statusHandler,
    /ap_error[\s\S]*?dns_error[\s\S]*?HA_TRANSPORT_SSID_CHANGE[\s\S]*?transport_pending_ssid\[0\]\s*=\s*'\\0'/,
    "AP or DNS failure discards a pending SSID before fallback up, even if fallback STATUS is lost");
  assert.match(session,
    /case HA_MSG_TRANSPORT_STATE:[\s\S]*?transport_network_ready[\s\S]*?ha_session_transport_resume/,
    "only a fresh TRANSPORT_STATE can auto-resume");
  assert.match(session,
    /transport_paused\s*&&\s*app->portal_running\s*&&\s*\n\s*app->transport_network_ready\s*&&[\s\S]*?transport_online_mask\s*==\s*app->transport_expected_mask/,
    "auto-resume requires a network-ready state packet and matching fresh masks");
  const waitHelper = session.slice(
    session.indexOf("bool ha_session_transport_wait_elapsed"),
    session.indexOf("// ---------------- STATUS handling"),
  );
  assert.match(waitHelper, /transport_host_deadline_set/,
    "an explicit armed bit makes wrapped deadline zero meaningful");
  assert.match(waitHelper,
    /\(int32_t\)\(furi_get_tick\(\)\s*-\s*app->transport_host_deadline\)\s*>=\s*0/,
    "host wait uses signed modular deadline comparison");
  const transportStateHandler = session.slice(
    session.indexOf("case HA_MSG_TRANSPORT_STATE"), session.indexOf("case HA_MSG_EVENT"));
  assert.match(transportStateHandler, /portal_running\s*=\s*\(p\[0\]\s*&\s*0x08\)\s*!=\s*0/,
    "authoritative state repairs a lost portal lifecycle STATUS");
  assert.match(transportStateHandler,
    /transport_paused\s*&&\s*!app->portal_running[\s\S]*?HA_TRANSPORT_SSID_CHANGE[\s\S]*?ha_session_network_restart/,
    "a down SSID-transition snapshot restarts the network if suspended STATUS was lost");
  assert.match(transportStateHandler,
    /portal_running\s*&&\s*app->transport_network_ready\s*&&\s*app->hs\s*==\s*HaHsStart\)\s*\n\s*app->hs\s*=\s*HaHsUp/,
    "a healthy state snapshot completes startup if up STATUS was lost");
  const authoritativeResumeStart = transportStateHandler.indexOf("if(!app->transport_paused)");
  const authoritativeResume = transportStateHandler.slice(
    authoritativeResumeStart,
    transportStateHandler.indexOf(
      "if(app->transport_paused && app->portal_running", authoritativeResumeStart,
    ),
  );
  assert.match(authoritativeResume, /transport_wait_expired\s*=\s*false/,
    "an authoritative unpaused snapshot dismisses the expired host prompt");
  assert.match(authoritativeResume, /transport_host_deadline_set\s*=\s*false/,
    "an authoritative unpaused snapshot disarms the reconnect deadline");
  assert.match(authoritativeResume, /transport_expected_mask\s*=\s*0/,
    "an authoritative unpaused snapshot clears stale reconnect masks");
  const expiryCheck = transportStateHandler.indexOf("ha_session_transport_wait_elapsed(app)");
  const autoResume = transportStateHandler.lastIndexOf("ha_session_transport_resume(app)");
  assert.ok(expiryCheck >= 0 && expiryCheck < autoResume,
    "exact deadline expiry is applied before considering mask-complete auto-resume");
  assert.match(transportStateHandler,
    /transport_network_ready\s*&&\s*!app->transport_wait_expired\s*&&[\s\S]*?transport_online_mask\s*==\s*app->transport_expected_mask/,
    "expired reconnect window disables mask-complete auto-resume");
  const explicitResume = session.slice(
    session.indexOf("void ha_session_transport_resume"),
    session.indexOf("bool ha_session_transport_wait_elapsed"),
  );
  assert.match(explicitResume, /HA_MSG_TRANSPORT_RESUME/,
    "explicit resume still sends the transport command");
  assert.doesNotMatch(explicitResume, /transport_wait_expired/,
    "expiry blocks only automatic resume, never the host's explicit action");
  assert.doesNotMatch(explicitResume, /strcmp[\s\S]*?transport_resuming[\s\S]*?return/,
    "a lost RESUME frame or acknowledgement can be retried explicitly");
  const pauseHelper = session.slice(
    session.indexOf("void ha_session_transport_pause"),
    session.indexOf("void ha_session_network_restart"),
  );
  assert.match(pauseHelper,
    /app->hs\s*!=\s*HaHsUp[\s\S]*?!app->portal_running[\s\S]*?!app->transport_network_ready/,
    "host transport mutation is rejected until startup and network health are complete");
  assert.match(ssidSource,
    /app->hs\s*==\s*HaHsUp\s*&&\s*app->portal_running\s*&&\s*app->transport_network_ready/,
    "SSID editor cannot race the startup SET_AP transaction");
  assert.match(appSource,
    /transport_paused\s*&&\s*!app->transport_wait_expired[\s\S]*?ha_session_transport_wait_elapsed/,
    "host tick marks the reconnect window expired without ending the session");
  assert.match(lobbySource,
    /transport_wait_expired\s*\?\s*"Resume"\s*:\s*"Games"/,
    "expired dashboard replaces Games with Resume");
  assert.match(lobbySource,
    /transport_wait_expired\s*\?\s*"End"\s*:\s*"Scores"/,
    "expired dashboard replaces Scores with End");
  assert.match(lobbySource,
    /GuiButtonTypeLeft[\s\S]*?HaEventTransportResume[\s\S]*?GuiButtonTypeRight[\s\S]*?HaEventTransportEnd/,
    "expired dashboard buttons dispatch explicit Resume and End actions");

  const model = {
    portal: true, pending: true, networkReady: false, expected: 0b11, online: 0b11,
    resumeCount: 0,
    statusUp() { this.portal = true; this.networkReady = false; },
    suspended() { this.portal = false; this.networkReady = false; },
    packet(networkReady, online) {
      this.networkReady = networkReady;
      this.online = online;
      if (this.portal && this.networkReady && this.online === this.expected) this.resumeCount++;
    },
  };
  model.packet(model.portal && !model.pending, model.online);
  assert.equal(model.resumeCount, 0, "pending pre-shutdown packet cannot resume");
  model.suspended();
  model.pending = false;
  model.statusUp();
  assert.equal(model.resumeCount, 0, "STATUS up cannot use stale matching masks");
  model.packet(true, 0b01);
  assert.equal(model.resumeCount, 0, "fresh but incomplete snapshot keeps waiting");
  model.packet(true, 0b11);
  assert.equal(model.resumeCount, 1, "fresh healthy complete snapshot resumes once");

  const lossyLifecycle = {
    paused: true, portal: true, ready: false, reason: 1, restarts: 0, hs: "up",
    packet(portal, ready) {
      this.portal = portal;
      this.ready = ready;
      if (portal && ready && this.hs === "start") this.hs = "up";
      if (this.paused && !portal && this.reason === 1) this.restarts++;
    },
  };
  // Drop network_suspended: its down snapshot still starts the replacement AP.
  lossyLifecycle.packet(false, false);
  assert.equal(lossyLifecycle.restarts, 1);
  // Drop the replacement up token too: the healthy state still restores liveness.
  lossyLifecycle.hs = "start";
  lossyLifecycle.packet(true, true);
  assert.equal(lossyLifecycle.portal, true);
  assert.equal(lossyLifecycle.ready, true);
  assert.equal(lossyLifecycle.hs, "up");
  // Conversely, STATUS up alone intentionally invalidates readiness. If its paired
  // state was the lost frame, the periodic snapshot supplies it without a phone event.
  lossyLifecycle.ready = false;
  lossyLifecycle.packet(true, true);
  assert.equal(lossyLifecycle.ready, true, "periodic state repairs a dropped up-adjacent state");

  const promoteHandshake = (phase, portal = true, ready = true) =>
    (portal && ready && phase === "start") ? "up" : phase;
  for (const phase of ["clear", "files", "content", "set_ap"]) {
    assert.equal(promoteHandshake(phase), phase,
      `live old-portal snapshot cannot skip in-flight ${phase} handshake`);
  }
  assert.equal(promoteHandshake("start"), "up",
    "healthy snapshot is authoritative only for the START portal-confirmation phase");

  const adapter = {
    portal: true, pending: false, shutdownArms: 0,
    pause(result) {
      if (result === "ok") {
        this.pending = this.portal;
        this.shutdownArms++;
      }
    },
  };
  adapter.pause("ok");
  assert.equal(adapter.shutdownArms, 1);
  adapter.pending = false; // recovered AP is now live after the first suspension/restart
  adapter.pause("already");
  assert.equal(adapter.pending, false, "duplicate PAUSE leaves recovered AP running");
  assert.equal(adapter.shutdownArms, 1, "duplicate PAUSE does not arm a second shutdown");

  const fallbackHost = {
    ssid: "OLD ARCADE", pendingSsid: "FAILED ARCADE", paused: true, reason: 1,
    status(token) {
      if ((token === "ap_error" || token === "dns_error") && this.paused && this.reason === 1)
        this.pendingSsid = "";
      if (token === "up" && this.pendingSsid) this.ssid = this.pendingSsid;
    },
  };
  fallbackHost.status("ap_error");
  // Drop the optional ap_fallback token, then deliver only the proven fallback's up.
  fallbackHost.status("up");
  assert.equal(fallbackHost.ssid, "OLD ARCADE",
    "an earlier AP failure prevents a lost fallback STATUS from persisting the failed candidate");

  const modularReached = (now, deadline) => (((now >>> 0) - (deadline >>> 0)) | 0) >= 0;
  const hostSnapshot = (now, deadline, deadlineSet = true) => {
    const expired = deadlineSet && modularReached(now, deadline);
    return {
      expired,
      autoResume: !expired, // all other gates and complete masks are true in this model
    };
  };
  assert.deepEqual(hostSnapshot(999, 1000), { expired: false, autoResume: true },
    "complete masks auto-resume before the deadline");
  assert.deepEqual(hostSnapshot(1000, 1000), { expired: true, autoResume: false },
    "exact deadline prompts instead of auto-resuming");
  assert.deepEqual(hostSnapshot(1001, 1000), { expired: true, autoResume: false },
    "post-deadline complete masks remain host-controlled");

  // 0xfffffff0 + 16 wraps to the valid deadline value 0. The armed bit, rather
  // than deadline != 0, distinguishes this from an uninitialized window.
  assert.deepEqual(hostSnapshot(0xffffffff, 0), { expired: false, autoResume: true },
    "wrapped complete masks auto-resume one tick before deadline zero");
  assert.deepEqual(hostSnapshot(0, 0), { expired: true, autoResume: false },
    "wrapped deadline zero expires at its exact tick");
  assert.deepEqual(hostSnapshot(1, 0), { expired: true, autoResume: false },
    "wrapped post-deadline masks cannot auto-resume");

  const expiredHost = {
    paused: true, portal: true, networkReady: true, expired: true, resumeSends: 0,
    explicitResume() {
      if (this.paused && this.portal && this.networkReady) this.resumeSends++;
    },
  };
  expiredHost.explicitResume();
  expiredHost.explicitResume();
  assert.equal(expiredHost.resumeSends, 2,
    "host Resume remains available and retryable after expiry");

  // STATUS is advisory; the following CRC-protected snapshot must complete host
  // recovery if `transport_resumed` itself was dropped on the UART link.
  const lostStatusHost = {
    paused: true,
    waitExpired: true,
    deadlineSet: true,
    expected: 0b11,
    online: 0b11,
    state(paused) {
      this.paused = paused;
      if (!paused) {
        this.waitExpired = false;
        this.deadlineSet = false;
        this.expected = 0;
        this.online = 0;
      }
    },
  };
  lostStatusHost.state(false);
  assert.deepEqual(lostStatusHost, {
    paused: false,
    waitExpired: false,
    deadlineSet: false,
    expected: 0,
    online: 0,
    state: lostStatusHost.state,
  }, "unpaused state alone clears the expired transport mirror");
}

console.log("transport-pause: OK");
