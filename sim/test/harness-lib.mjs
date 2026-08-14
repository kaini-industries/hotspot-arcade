// Thin ergonomic wrapper over the exported C API, shared by the headless tests.
import createEngine from "../web/engine.js";

export async function newEngine() {
  const M = await createEngine();
  const drain = () => JSON.parse(M.ccall("ha_drain", "string", [], []));
  const api = {
    drain,
    reset: () => { M.ccall("ha_reset", null, [], []); return drain(); },
    resetAt: (ms) => { M.ccall("ha_reset_at", null, ["number"], [ms]); return drain(); },
    setAdmissionFull: (full) => {
      M.ccall("ha_set_admission_full", null, ["number"], [full ? 1 : 0]);
      return drain();
    },
    tick: (ms) => { M.ccall("ha_tick", null, ["number"], [ms]); return drain(); },
    input: (wsId, obj) => {
      M.ccall("ha_input", null, ["number", "string"], [wsId, JSON.stringify(obj)]);
      return drain();
    },
    inputRaw: (wsId, json) => {
      M.ccall("ha_input", null, ["number", "string"], [wsId, json]);
      return drain();
    },
    inputAt: (wsId, obj, ms) => {
      M.ccall("ha_input_at", null, ["number", "string", "number"], [wsId, JSON.stringify(obj), ms]);
      return drain();
    },
    disconnect: (wsId) => { M.ccall("ha_disconnect", null, ["number"], [wsId]); return drain(); },
    timeReached: (now, deadline) =>
      M.ccall("ha_time_reached", "number", ["number", "number"], [now, deadline]) !== 0,
    timeRemaining: (now, deadline) =>
      M.ccall("ha_time_remaining", "number", ["number", "number"], [now, deadline]) >>> 0,
    selectGame: (id) => { M.ccall("ha_select_game", null, ["number"], [id]); return drain(); },
    roundEnd: () => { M.ccall("ha_round_end", null, [], []); return drain(); },
    resetScores: () => { M.ccall("ha_reset_scores", null, [], []); return drain(); },
    setLang: (lang) => { M.ccall("ha_set_lang", null, ["string"], [lang || ""]); return drain(); },
    triviaClear: () => { M.ccall("ha_trivia_clear", null, [], []); return drain(); },
    triviaAddTopic: (name) => { M.ccall("ha_trivia_add_topic", null, ["string"], [name]); return drain(); },
    triviaAddQ: (json) => { M.ccall("ha_trivia_add_q", null, ["string"], [json]); return drain(); },
    contentClear: () => { M.ccall("ha_content_clear", null, [], []); return drain(); },
    contentPack: (game, name) => {
      M.ccall("ha_content_pack", null, ["number", "string"], [game, name]);
      return drain();
    },
    contentItem: (json) => { M.ccall("ha_content_item", null, ["string"], [json]); return drain(); },
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
  api.join = (wsId, nick, resume = undefined, code = undefined) => {
    const token = resume || wsId.toString(16).padStart(32, "0").slice(-32);
    return api.input(wsId, {
      t: "hello", proto: 2, nick, avatar: "🙂", resume: token,
      ...((code === null) ? {} : { code: code === undefined ? "123456" : code }),
    });
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
