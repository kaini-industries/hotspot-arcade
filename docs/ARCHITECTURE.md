# Architecture

Hotspot Arcade is three programs cooperating over two links:

```
 Phones (browser)        ESP32-S2 board            Flipper Zero
 -----------------       ----------------          ------------------
 web/ game client  <ws>  esp32/hotspot-arcade-fw   flipper/hotspot-arcade
   trivia.js             AP + wildcard DNS         host UI (scenes)
   connect4.js           catch-all HTTP (assets)   UART v2 (ha_uart)
   app.js (core)         AsyncWebSocket /ws        session/roster/rounds
                         game engine (referee)     content/session host
                              ^                          ^
                              +------ UART 921600 -------+
```

## The split: why the work lands where it does

The ESP32-S2 (240 MHz) is faster than the Flipper (64 MHz) **and** it is where the
phone sockets terminate, so all **real-time** state lives on the ESP. The Flipper has
the screen, the buttons, and the SD card, so it owns the **session/meta** layer. The
UART only ever carries low-frequency, high-level messages, never per-frame state.

| Flipper owns (session/meta)                         | ESP owns (real-time)                    |
|-----------------------------------------------------|-----------------------------------------|
| Stable-identity roster + cumulative host scoreboard | WebSocket connections, transient resumable seats |
| Which game is active and host transport lifecycle   | Per-move / per-question game state       |
| Content banks streamed transactionally from SD      | Move validation, clocks, round flow      |
| Authoritative persistent scores, host display        | Broadcasting state to phones            |
| Streaming the web bundle to the ESP                  | Serving the bundle from RAM             |

Scores are computed by the ESP (it is the referee), broadcast to phones for the selected
game, and reported as signed deltas via `SCORE` so the host can maintain a cumulative
identity-keyed session ledger. Bounded typed `EVENT` records drive the host feed; v18 no
longer emits game-shaped `ROUND_RESULT` JSON.

## ESP32 firmware (`esp32/hotspot-arcade-fw/`)

Single Arduino sketch plus header-only helpers (one translation unit):

- `hotspot-arcade-fw.ino` — AP/DNS/HTTP bring-up, the WebSocket `/ws` handler, the
  framed UART RX/TX, and the engine "sink" implementations. Two FreeRTOS mutexes:
  `serialMutex` serializes whole UART frames (emitted from the loop and async tasks),
  `engineMutex` (recursive) guards engine state touched from both tasks.
- `ha_assets.h` — in-RAM file table. The Flipper streams gzipped files in; the HTTP
  catch-all serves them (with `Content-Encoding: gzip`). No filesystem, so nothing
  survives a reboot; the Flipper re-streams on the next session.
- `ha_games.h` — the engine: player roster (with emoji avatars and SHA-256-derived identities;
  raw browser resume tokens are never retained)
  plus all fifteen games and
  their per-client JSON serialization. The whole-group games (Trivia, Would You Rather,
  Word Scramble, Reaction Duel, Guess the Color, Spectrum, Kiss Marry Kill) are
  phone-driven and self-organizing (ready-up -> countdown -> rounds -> podium);
  Connect Four / Tic-Tac-Toe / Dots & Boxes / Reversi share one generalized duel +
  challenge system (parameterized by kind), and Pong, Battleship, and Chess reuse the
  same challenge flow with their own match structs (Chess runs full server-side FIDE
  legality plus 5+0 blitz clocks); Drawing rotates a drawer and relays ink. Pong,
  Reaction Duel, and the chess clocks run on a uint32 logical clock advanced by `tick()`
  (called from the `.ino` loop)
  alongside the trivia/party/draw round timers. Emoji reactions broadcast to everyone
  as a `emoji` message. Trivia and the duels are event-driven; Pong is the real-time path.
  The logical clock freezes while the portal is stopped and uses rollover-safe signed
  deadline comparisons. Raw uptime separately measures the 120-second reconnect grace.
  Player and match pool sizes are compile-time caps (`HA_MAX_PLAYERS`,
  `DUEL_MAX_MATCHES`, `PONG_MAX`, `BATTLE_MAX`, `CHESS_MAX`); UART `CONFIG.max`
  controls the live AP/WebSocket cleanup cap within the compiled player bound.
  Since only one game is active, its 1v1 match tables share one union. String-heavy
  content tables are allocated per game only when packs are streamed. Replacement is
  staged in a heap-backed bank and atomically swapped only by a count-verified
  `CONTENT_COMMIT`; chess repetition keys are derived instead of held in mutable DRAM.
- `ha_json.h` / `ha_proto.h` — tiny JSON reader/writer and the UART frame constants +
  CRC-8.

The web app is streamed into RAM (about 39 KB gzipped), so RAM stays flat regardless of
trivia pack size (questions are pushed one at a time, never stored in bulk).

## Flipper app (`flipper/hotspot-arcade/`)

A `ViewDispatcher` + `SceneManager` app, same shape as flytrap:

- `hotspot_arcade.c` — alloc/free, the UART-worker -> GUI custom-event bridge, and a 1s
  liveness tick (the ESP beacons `PING` ~every 2s; silence for 5s flags a lost board).
- `ha_uart.c` — the race-free UART transport (IRQ -> stream buffer -> worker -> GUI-thread
  parse), including the mandatory `expansion_disable()` dance before acquiring the GPIO
  USART. Runs at 921600.
- `ha_proto.c` — framed message encode.
- `helpers/ha_session.c` — the heart: the RX frame parser, the start **handshake**
  state machine (CLEAR_FILES -> stream bundle -> transactional content -> SET_AP -> START,
  driven by ESP acks), the stable-identity roster (JOIN/LEAVE/SCORE), and the bounded typed
  event formatter. Live language changes rerun the content transaction and are reported
  successful only after `content_ok`.
- `helpers/ha_storage.c` — config (FlipperFormat), `manifest.json` parsing, binary-safe
  file reads (pre-reserved buffers to avoid an OOM-inducing 2x realloc peak), trivia
  pack loading.
- `scenes/` — main_menu, lobby (dashboard + start flow), game_select, host_duel (event feed
  for the player-driven games), leaderboard, settings, ssid_input, flasher, textview
  (console). Every game is phone-driven, so the Flipper only selects the game and watches
  the feed — there is no per-game host screen.
- `helpers/ha_esp_port.c` + `helpers/ha_flasher.c` + `scenes/..._flasher.c` — an on-device
  ESP flasher over the GPIO UART (Espressif `esp-serial-flasher`, Apache-2.0, vendored in
  `lib/esp-serial-flasher/` trimmed to the ESP32-S2 and ESP32 WROOM stubs). It borrows the serial line via
  `ha_uart_suspend`/`resume`, polls for download mode, flashes the SD firmware bundle with
  MD5 verify on a worker thread, and reboots the ESP into the new firmware.

All app state is single-threaded (mutated only on the GUI thread after RX is drained),
so there are no locks on the Flipper side.

## Web client (`web/`)

Vanilla JS, no framework, built into a single gzipped `index.html`. `app.js`
owns the WebSocket and browser identity (nickname + emoji avatar + a Web Crypto-generated
resume token kept only in localStorage), lobby, screen
router, planned-pause/reconnect state, emoji reactions, and the shared game-UI components (`A.readyLobby` / `A.countdown`
/ `A.timebar` / `A.showLead` leaderboard / `A.podium`). Each game module (`trivia.js`,
`duel.js` for the four board duels, `draw.js`, `pong.js`, `wyr.js`, `scramble.js`,
`react.js`) registers handlers for its message types and reuses those components. User-facing text is
localized through `core/i18n.js`: the host's language rides in `welcome` and live
`config` broadcasts, and the client renders from a message catalog, with English the
default and the fallback for any untranslated string. Roster views retain disconnected
seats as visibly offline, while challenge lists exclude them. Styled to the Flipper design
system ([../web/DESIGN.md](../web/DESIGN.md)): dark, monochrome, one orange accent,
mono/uppercase, sharp borders. The captive page is a real-browser handoff because iOS/
Android captive mini-browsers do not run WebSockets reliably.

## Wire protocols

Both links are specified in [PROTOCOL.md](PROTOCOL.md): the framed UART v2 (with the
raw-bulk escape used to stream files) and the WebSocket JSON. The protocol is the source
of truth; all three programs are kept in sync with it.

## Data flow: a trivia round

1. Host picks a pack (SD) and Start Session. Flipper streams the bundle to the ESP,
   which brings up the AP. Phones join with WebSocket protocol v2: `hello{proto:2,...}`
   -> `welcome{session,...}`/`lobby`. A transient disconnect reserves that seat for 120s;
   reconnecting with the token restores the same pid, score, and game state.
2. The host selects Trivia. The already-committed content bank is authoritative; phones
   ready up and the engine starts the countdown/round itself.
3. ESP broadcasts relative-timer `trivia` state; phones submit intents; ESP validates,
   scores, reports every delta through `SCORE`, and surfaces bounded typed host events.
4. The engine reveals and advances rounds from its logical clock. A normal disconnect
   reserves exact state for 120 seconds; a planned transport pause freezes all games and
   defers quorum reconciliation until explicit resume.
