// Thin ergonomic wrapper over the exported C API, shared by the headless tests.
import createEngine from "../web/engine.js";

export async function newEngine() {
  const M = await createEngine();
  const drain = () => JSON.parse(M.ccall("ha_drain", "string", [], []));
  let legacy = null;
  const flushLegacy = () => {
    if (!legacy || legacy.game === null) return [];
    M.ccall("ha_content_commit", "number", ["number", "number"], [legacy.packs, legacy.items]);
    legacy = null;
    return drain();
  };
  const api = {
    drain,
    reset: () => { legacy = null; M.ccall("ha_reset", null, [], []); return drain(); },
    resetAt: (ms) => { legacy = null; M.ccall("ha_reset_at", null, ["number"], [ms]); return drain(); },
    setAdmissionFull: (full) => {
      M.ccall("ha_set_admission_full", null, ["number"], [full ? 1 : 0]);
      return drain();
    },
    tick: (ms) => {
      const before = flushLegacy();
      M.ccall("ha_tick", null, ["number"], [ms]);
      return before.concat(drain());
    },
    input: (wsId, obj) => {
      const before = flushLegacy();
      M.ccall("ha_input", null, ["number", "string"], [wsId, JSON.stringify(obj)]);
      return before.concat(drain());
    },
    inputRaw: (wsId, json) => {
      const before = flushLegacy();
      M.ccall("ha_input", null, ["number", "string"], [wsId, json]);
      return before.concat(drain());
    },
    inputAt: (wsId, obj, ms) => {
      const before = flushLegacy();
      M.ccall("ha_input_at", null, ["number", "string", "number"], [wsId, JSON.stringify(obj), ms]);
      return before.concat(drain());
    },
    disconnect: (wsId) => {
      const before = flushLegacy();
      M.ccall("ha_disconnect", null, ["number"], [wsId]);
      return before.concat(drain());
    },
    transportPause: (reason, ssid = "", reconnectMs = 0) => {
      const result = M.ccall(
        "ha_transport_pause", "number", ["number", "string", "number"],
        [reason, ssid, reconnectMs],
      );
      return { result, out: drain() };
    },
    transportResume: (expireMissing = false) => {
      const result = M.ccall("ha_transport_resume", "number", ["number"], [expireMissing ? 1 : 0]);
      return { result, out: drain() };
    },
    transportDetachSockets: () => {
      M.ccall("ha_transport_detach_sockets", null, [], []);
      return drain();
    },
    transportFallbackSsid: (ssid) => {
      const ok = M.ccall("ha_transport_fallback_ssid", "number", ["string"], [ssid]) !== 0;
      return { ok, out: drain() };
    },
    transportPaused: () => M.ccall("ha_transport_paused", "number", [], []) !== 0,
    transportExpected: () => M.ccall("ha_transport_expected", "number", [], []) >>> 0,
    transportOnlineExpected: () =>
      M.ccall("ha_transport_online_expected", "number", [], []) >>> 0,
    sessionNow: () => M.ccall("ha_session_now", "number", [], []) >>> 0,
    gameNow: () => M.ccall("ha_game_now", "number", [], []) >>> 0,
    timeReached: (now, deadline) =>
      M.ccall("ha_time_reached", "number", ["number", "number"], [now, deadline]) !== 0,
    timeRemaining: (now, deadline) =>
      M.ccall("ha_time_remaining", "number", ["number", "number"], [now, deadline]) >>> 0,
    selectGame: (id) => {
      if (legacy && legacy.game === id) return flushLegacy();
      const before = flushLegacy();
      M.ccall("ha_select_game", "number", ["number"], [id]);
      return before.concat(drain());
    },
    roundEnd: () => {
      const before = flushLegacy();
      M.ccall("ha_round_end", null, [], []);
      return before.concat(drain());
    },
    resetScores: () => { M.ccall("ha_reset_scores", null, [], []); return drain(); },
    testSetScore: (pid, score) => {
      M.ccall("ha_test_set_score", null, ["number", "number"], [pid, score]);
      return drain();
    },
    testAwardScore: (pid, delta) => {
      M.ccall("ha_test_award_score", null, ["number", "number"], [pid, delta]);
      return drain();
    },
    testHostEvent: (kind, text) => {
      M.ccall("ha_test_host_event", null, ["number", "string"], [kind, text]);
      return drain();
    },
    contentBegin: (game, lang = "") => {
      legacy = null;
      const ok = M.ccall("ha_content_begin", "number", ["number", "string"], [game, lang]) !== 0;
      return { ok, out: drain() };
    },
    contentPack: (game, name) => {
      if (legacy) {
        if (legacy.game === null) {
          legacy.game = game;
          const began = M.ccall("ha_content_begin", "number", ["number", "string"], [game, legacy.lang]) !== 0;
          if (!began) return { ok: false, out: drain() };
        } else if (legacy.game !== game) {
          return { ok: false, out: drain() };
        }
      }
      const ok = M.ccall("ha_content_pack", "number", ["number", "string"], [game, name]) !== 0;
      if (ok && legacy) legacy.packs++;
      return { ok, out: drain() };
    },
    contentItem: (json) => {
      const ok = M.ccall("ha_content_item", "number", ["string"], [json]) !== 0;
      if (ok && legacy) legacy.items++;
      return { ok, out: drain() };
    },
    contentCommit: (packs, items) => {
      legacy = null;
      const ok = M.ccall("ha_content_commit", "number", ["number", "number"], [packs, items]) !== 0;
      return { ok, out: drain() };
    },
    contentAbort: () => { legacy = null; M.ccall("ha_content_abort", null, [], []); return drain(); },
    contentClear: () => {
      M.ccall("ha_content_abort", null, [], []);
      legacy = { game: null, lang: "", packs: 0, items: 0 };
      return drain();
    },
    contentFailAfter: (checkpoints) => {
      M.ccall("ha_content_fail_after", null, ["number"], [checkpoints]);
      return drain();
    },
    contentBankCount: () => M.ccall("ha_content_bank_count", "number", [], []),
    contentBankMax: () => M.ccall("ha_content_bank_max", "number", [], []),
    contentActiveGame: () => M.ccall("ha_content_active_game", "number", [], []),
    contentActiveLang: () => M.ccall("ha_content_active_lang", "string", [], []),
    // HA_CHESS_TEST-only hooks: load an arbitrary position into match slot 0 (must
    // already be a live game from challenge/accept), and perft a scratch position
    // against the real move generator.
    chessLoad: (board64, stm, rights, ep, halfmove, wms, bms) => {
      M.ccall(
        "ha_chess_load", null,
        ["string", "number", "number", "number", "number", "number", "number"],
        [board64, stm, rights, ep, halfmove, wms, bms],
      );
      return drain();
    },
    chessPerft: (board64, stm, rights, ep, depth) =>
      M.ccall(
        "ha_chess_perft", "number",
        ["string", "number", "number", "number", "number"],
        [board64, stm, rights, ep, depth],
      ),
  };
  api.triviaClear = api.contentClear;
  api.triviaAddTopic = (name) => api.contentPack(1, name).out;
  api.triviaAddQ = (json) => api.contentItem(json).out;
  api.join = (wsId, nick, resume = undefined, code = undefined) => {
    const token = resume || wsId.toString(16).padStart(32, "0").slice(-32);
    return api.input(wsId, {
      t: "hello", proto: 2, nick, avatar: "🙂", resume: token,
      ...((code === null) ? {} : { code: code === undefined ? "123456" : code }),
    });
  };
  api.loadContent = (game, packs = [], lang = "") => {
    let step = api.contentBegin(game, lang);
    if (!step.ok) throw new Error(`contentBegin failed for game ${game}`);
    let itemCount = 0;
    for (const pack of packs) {
      step = api.contentPack(game, pack.name);
      if (!step.ok) throw new Error(`contentPack failed for game ${game}: ${pack.name}`);
      for (const item of pack.items || []) {
        const json = typeof item === "string" ? item : JSON.stringify(item);
        step = api.contentItem(json);
        if (!step.ok) throw new Error(`contentItem failed for game ${game}: ${json}`);
        itemCount++;
      }
    }
    const committed = api.contentCommit(packs.length, itemCount);
    if (!committed.ok) throw new Error(`contentCommit failed for game ${game}`);
    return committed.out;
  };
  return api;
}

/** Last broadcast (to:"all") whose msg.t equals `type`, or undefined. */
export function lastBroadcast(items, type) {
  return items.filter((o) => o.to === "all" && o.msg && o.msg.t === type).pop();
}

/**
 * Last unicast (to:"ws") sent to `wsId` whose msg.t equals `type`, or undefined.
 *
 * Most engine state pushes (trivia/duel/lobby/...) go through pushAll(), which
 * calls haWsSendWs() once per connected socket rather than haWsBroadcast() -- so
 * `lastBroadcast` above never matches them. This is the helper the per-player
 * game tests actually need.
 */
export function lastToWs(items, wsId, type) {
  return items
    .filter((o) => o.to === "ws" && o.id === wsId && o.msg && o.msg.t === type)
    .pop();
}

/** Challenge id allocated in a lobby push, optionally narrowed to its recipient. */
export function challengeId(items, to = undefined) {
  for (let i = items.length - 1; i >= 0; i--) {
    const cs = items[i]?.msg?.challenges;
    if (!Array.isArray(cs)) continue;
    const c = [...cs].reverse().find((x) => to === undefined || x.to === to);
    if (c) return c.id;
  }
  return undefined;
}
