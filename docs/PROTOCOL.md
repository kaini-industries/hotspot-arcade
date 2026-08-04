# Hotspot Arcade — wire protocols

Two links, two protocols. This file is the source of truth; the Flipper app, the
ESP32 firmware, and the web client must all agree with it.

```
 Flipper Zero  <--- UART v2 (framed) --->  ESP32-S2  <--- WebSocket (JSON) --->  Phones
  host brain                                referee                               players
```

- **UART v2** carries only low-frequency session/meta traffic (asset upload, AP
  control, round orchestration, join/leave, score deltas). Never per-frame state.
- **WebSocket JSON** carries the real-time game traffic, local to the ESP.

---

## 1. UART v2 (Flipper <-> ESP32)

- **115200-safe wiring, run at 921600 8N1** on the Flipper GPIO USART (pins 13/14).
  Fallback 460800 if the trace is noisy. Baud is a build constant on both sides.
- Same expansion-service dance as flytrap: `expansion_disable()` **before**
  `furi_hal_serial_control_acquire()`, `expansion_enable()` after release.

### 1.1 Control frame

All control messages are framed so the link can resync after noise:

```
+------+------+---------+-----------------+------+
| SYNC | TYPE | LEN(2)  | PAYLOAD(LEN)    | CRC8 |
+------+------+---------+-----------------+------+
 0xA5    1B    LE u16     LEN bytes         1B
```

- `SYNC = 0xA5`.
- `TYPE` = one of the message types below.
- `LEN` = payload length, little-endian u16 (0..4096). Payloads are capped at
  **4096** bytes; larger data (asset files) uses the raw-bulk escape (1.3).
- `CRC8` = CRC-8/ATM (poly 0x07, init 0x00) over `TYPE || LEN || PAYLOAD`.
- On a bad CRC or unknown type, the receiver drops the frame and rescans for the
  next `0xA5`.

### 1.2 Message types

**Flipper -> ESP**

| Type | Name         | Payload |
|------|--------------|---------|
| 0x10 | CLEAR_FILES  | (none) — drop all stored assets, start of session |
| 0x11 | FILE_BEGIN   | `flags(1)` `pathlen(1)` `path` `mimelen(1)` `mime` `total(4 LE)` — then `total` **raw** bytes follow (see 1.3). `flags` bit0 = gzip. |
| 0x12 | SET_AP       | `ssid` (UTF-8, <=32B) |
| 0x13 | START        | (none) — bring up AP + DNS + HTTP + WS |
| 0x14 | STOP         | (none) — stop transport and freeze logical game time; roster/game state is retained for the next `START` |
| 0x15 | RESET        | (none) — ESP reboots |
| 0x16 | SELECT_GAME  | `gameid(1)` — 0 lobby; game ids 1..15 are defined in `ha_proto.h` |
| 0x17 | QUESTION     | Reserved legacy host-driven trivia command. Protocol v18 ignores it. |
| 0x18 | REVEAL       | Reserved legacy host-driven trivia command. Protocol v18 ignores it. |
| 0x19 | ROUND_END    | (none) — back to lobby for the active game |
| 0x1A | CONFIG       | JSON: `{"max":8,"lang":"pt-br","code":"123456"}` — station/WebSocket cleanup cap, phone-UI language, and optional six-digit admission code. The ESP echoes/broadcasts only `lang`; it never sends or logs the code. |
| 0x1B | RESET_SCORES | (none) — zero the ESP live score mirror |
| 0x1C | CONTENT_CLEAR | (none) — begin a staged replacement; the current content remains live |
| 0x1D | CONTENT_PACK | game byte + pack name — begin a pack in the staging bank |
| 0x1E | CONTENT_ITEM | JSON object of the file's own keys — append one item to the staged pack |
| 0x1F | CONTENT_COMMIT | `pack_count(2 LE)` `item_count(2 LE)` — publish the staged bank only when both accepted counts match exactly |

> Content is opaque to the Flipper. It parses only `Key: value` blocks and ships them
> verbatim; every game's interpretation of those keys lives in the ESP firmware, so a new
> content game needs no protocol change. Per-game item shapes (no new opcodes, just what
> the ESP expects in each `CONTENT_ITEM` JSON object): trivia `{q,a,b,c,d,answer}`, wyr
> `{a,b}` (the two options), scramble and draw `{word}` (a single plain word).
>
> Content **language** is resolved entirely on the Flipper: for the host's chosen `lang`
> it streams `packs/<game>/<lang>/` (falling back to the English packs at `packs/<game>/`
> per game), so the wire opcodes above are language-agnostic. Item text is UTF-8.
> A live locale replacement is one transaction: `CONTENT_CLEAR`, `CONFIG`, every
> `CONTENT_PACK`/`CONTENT_ITEM`, then the four-byte `CONTENT_COMMIT`. Firmware stages
> `lang` while the transaction is open, returns the selected game to its lobby, broadcasts
> the new `config`, then emits one authoritative state. A malformed item, allocation
> failure, missing frame, count mismatch, or malformed commit discards staging and keeps
> both the previous bank and locale. The host waits for `STATUS content_ok`; `content_error`
> is failure and must not be reported as a successful language change.

**ESP -> Flipper**

| Type | Name         | Payload |
|------|--------------|---------|
| 0x80 | STATUS       | token including `boot`, `cleared`, `fok`, `ap_ok`, `ap_set`, `content_ok`, `content_error`, `up ip=..`, `stopped`, or `config_error` |
| 0x81 | JOIN         | `pid(1)` `identity(16)` `nick` — stable identity is the first 128 bits of SHA-256 over the browser-only resume token |
| 0x82 | LEAVE        | `pid(1)` |
| 0x83 | SCORE        | `pid(1)` `delta(2 LE, signed)` `reason` — authoritative-persist on Flipper |
| 0x84 | ROUND_RESULT | Reserved legacy v17 JSON result. Protocol v18 producers do not send it. |
| 0x85 | EVENT        | Typed bounded event: `version(1)` `kind(1)` `game(1)` `actor_pid(1)` `target_pid(1)` `value(2 LE, signed)` `text(0..96 UTF-8 bytes)` |
| 0x86 | PING         | identity beacon ~every 2s: `magic(4)` + `version(2 LE)`. `magic` = `48 41 52 43` ("HARC"); the Flipper only treats a magic-matched PING as "our board present", and flags `version < HA_FW_VERSION` as an outdated board to update. |

`EVENT.version` is currently `1`. Unknown versions or kinds are rejected by consumers;
the frame remains bounded and game-independent:

| Kind | Name | Field use |
|------|------|-----------|
| 1 | `MATCH_STARTED` | `game`, `actor_pid`, and `target_pid` identify the pairing |
| 2 | `CHAT` | `actor_pid` sent `text` |
| 3 | `ROLE` | `actor_pid` received or performed the role described by `text` |
| 4 | `ROUND_WIN` | `actor_pid` beat `target_pid`; optional `value`/`text` qualify the result |
| 5 | `ROUND_DRAW` | `actor_pid` and `target_pid` drew; optional `value`/`text` qualify it |
| 6 | `ROUND_COMPLETE` | `value` is the completed round number; actor/target are optional |
| 7 | `GAME_FINAL` | The selected `game` reached its final state |

Text is trimmed only at a complete UTF-8 code-point boundary. All game awards travel
separately as `SCORE`; a consumer must never infer score deltas from display events.

### 1.3 Raw-bulk escape (asset upload)

`FILE_BEGIN` is a normal control frame; immediately after its CRC, the sender
writes exactly `total` **unframed** bytes (the file content, possibly gzipped).
The receiver switches to a raw-read state, counts down `total`, stores the bytes,
then returns to frame parsing. This mirrors flytrap's `sethtml <N>\n` + N bytes,
generalized to named files. Bulk bytes need no escaping because the length is known.

### 1.4 Handshake (session start)

```
Flipper                         ESP
  |-- CLEAR_FILES -------------->|
  |-- FILE_BEGIN + bytes (xN) -->|   (web bundle)
  |-- CONTENT_CLEAR ------------>|
  |-- CONFIG ------------------->|   (lang + six-digit join code)
  |-- CONTENT_PACK/ITEM (xN) --->|
  |-- CONTENT_COMMIT ----------->|   (accepted pack/item counts)
  |<---------- STATUS content_ok-|
  |-- SET_AP ------------------->|
  |-- START -------------------->|
  |<------------- STATUS ap_ok --|
  |<------------- STATUS up ip=..|   AP live, phones can join
```
The content transaction is sent before `SET_AP`; its `content_ok` acknowledgement is
required. Then live: JOIN/LEAVE/SCORE/typed EVENT flow up; CONFIG/SELECT_GAME/ROUND_END
flow down. PING beacons throughout. `STOP`
does not end player identities: it freezes game time, closes transport, and `START`
gives detached seats a fresh reconnect window. `RESET`/reboot is the hard reset.

---

## 2. WebSocket JSON v2 (Phone <-> ESP32)

- Endpoint: `ws://192.168.4.1/ws`. One socket per phone.
- All messages are a single JSON object with a `t` (type) field. Small; one frame.
- The ESP is authoritative: clients render server state and send intents only.

### 2.1 Client -> Server

| `t`        | Fields | Meaning |
|------------|--------|---------|
| `hello`    | `proto:2`, `resume`, `nick`, `avatar`, optional `code` | The browser creates and persists a 128-bit lowercase-hex `resume` token using `crypto.getRandomValues`. Unknown identities need the six-digit host code; a known identity may omit it. |
| `answer`   | `c` (0-3) | Trivia: buzz an answer for the current question |
| `challenge`| `to` (pid) | Connect4: challenge a player in the lobby |
| `accept`   | `id` (challenge id) | Accept exactly one pending challenge; sender pid alone is not an identity. |
| `cancel`   | | Connect4: withdraw my challenge / decline |
| `move`     | `n` | Game-specific move index (or Chess `from`/`to` fields) |
| `leaveGame`| | Connect4: forfeit/exit the current match |
| `ping`     | | keepalive |

### 2.2 Server -> Client

| `t`      | Fields | Meaning |
|----------|--------|---------|
| `welcome`| `proto:2`, `session`, `pid`, `nick`, `avatar`, `lang`, `resumed`, `paused` | Seat identity after `hello`; `session` is a 128-bit lowercase-hex server session id. `resumed` means an exact engine-seat resume, not merely a host-known identity. The browser token is never echoed. |
| `reject` | `code` (`auth_required`/`bad_code`/`throttled`/`full`/`bad_protocol`), optional `retry_ms`/`proto` | The hello was not accepted and allocated no seat. |
| `error`  | `code` (`challenge_capacity`/`match_capacity`) | A bounded challenge or match table is full; no match was created. |
| `config` | `lang` | Live host configuration change; clients re-render localized UI without reconnecting. |
| `server_pause` | `reason`, `ssid`, `reconnect_ms` | Planned transport downtime. The client freezes local timers and gameplay, keeps reconnecting, and displays the host-selected SSID/window. Re-sent after every hello while paused. |
| `server_resume` | | Explicitly unfreezes the client. A socket that had not yet received `welcome` retries `hello`. |
| `lobby`  | `game`, `players` (`[{pid,nick,avatar,score,online,busy}]`), `me` | Lobby snapshot. Detached-but-reserved players remain listed with `online:false` and cannot be challenged. |
| `trivia` | `phase` ("idle"/"question"/"reveal"), `i`, `q`, `o` (opts), `remaining_ms`, `duration_ms`, `mine` (my choice or -1), `counts` ([n0..n3]), `correct` (reveal only), `scores` | Full trivia view for this client |
| `duel`   | `phase` ("lobby"/"playing"/"over"), lobby `challenges:[{id,from,to}]`; playing/over fields are game-specific | Full shared duel view for this client. Pong, Battleship, and Chess use the same challenge objects in their own lobby messages. |
| `toast`  | `msg` | Transient message to show |
| `pong`   | | keepalive reply |

Timed state exposes only relative `remaining_ms` and `duration_ms`. Raw ESP deadlines
and uptime never cross the WebSocket protocol. Internally the engine uses a logical
clock that freezes while transport is stopped and rollover-safe signed comparisons;
the server remains authoritative.

### 2.3 Resume and disconnect lifecycle

- The browser creates its random 128-bit `resume` token. The engine derives
  `SHA-256(resume)[0..15]` and stores/reports only that identity digest; raw tokens remain
  browser-only. An unknown digest must present the current host join code.
- A socket disconnect marks that player `online:false` but retains pid, score, readiness,
  and live match state for **120 seconds**. It does not emit UART `LEAVE` or forfeit yet.
- A v2 `hello` with the exact token within the grace window rebinds the seat. If two
  sockets claim it, the newest binding wins and input from the old socket no longer maps
  to a player.
- At 120 seconds the seat is finalized: live matches receive the normal leave/forfeit,
  per-player game arrays are cleared, and UART `LEAVE` is emitted. A permanent host
  ledger may still recognize the digest later and allocate a fresh pid/game score.
- Outstanding challenges are deliberately shorter lived than seats: either endpoint
  disconnecting, leaving, changing games, or entering a match removes them. Accept uses
  the challenge `id`, and an exhausted match pool returns `error{code:"match_capacity"}`.
- Planned transport pause is different: logical time, quorum reconciliation, challenges,
  match state, and expiry are frozen. Reserved or restored host-known identities may bind
  while paused; genuinely new admission waits for `server_resume`. When the host resumes,
  presence is reconciled once. A still-missing 1v1 opponent or role-critical player then
  enters ordinary transient grace; unrelated party quorum excludes offline players.

### 2.4 Scoring split

The ESP owns the score for the currently selected phone game. Selecting another game
or starting that game's replay resets this browser-facing score without emitting a
negative award. Every positive or negative award is also emitted exactly once through
UART `SCORE`, keyed by the current pid; the host resolves that pid to the stable identity
from `JOIN` and adds the delta to its cumulative session ledger. Content/locale replacement
preserves both values while returning only the active round to its lobby.

---

## 3. v0.2 game expansion

New game ids (UART `SELECT_GAME` / lobby `game`): `3` tictactoe, `4` dots,
`5` draw, `6` pong. Lobby `game` string adds: `"tictactoe"`, `"dots"`, `"draw"`,
`"pong"`.

### 3.1 Duels (connect4, tictactoe, dots, reversi) — unified

All four are 1v1 and share the same lobby flow. Client intents:
`challenge{to}`, `accept{id}`, `cancel`, `move{n}`, `rematch`, `leaveGame`.
`move.n` is a grid index whose meaning depends on `kind` (below). `rematch` in an
`over` match restarts the same pairing (first move alternates) if the opponent is
still present.

Server -> client message `t:"duel"`, common fields: `kind`
("c4"/"ttt"/"dots"), `phase` ("lobby"/"playing"/"over"), `you` (pid), `me`
(1 or 2), `opp` (nick), `turn` (pid), `result` ("win"/"lose"/"draw", over only),
`challenges` (`[{id,from,to}]`, lobby only).

- **c4** (`kind:"c4"`): `cols:7`, `rows:6`, `need:4`, `gravity:true`, `board`
  (42 ints, row-major, row 0 top, 0/1/2). `move.n` = column 0..6.
- **ttt** (`kind:"ttt"`): `cols:3`, `rows:3`, `need:3`, `gravity:false`, `board`
  (9 ints). `move.n` = cell 0..8.
- **dots** (`kind:"dots"`): boxes grid `w`,`h` (e.g. 5x5 boxes). `hedges`
  (`(h+1)*w` ints 0/1 = drawn), `vedges` (`h*(w+1)` ints), `boxes`
  (`w*h` ints 0/1/2 = owner). `sme`,`sopp` (box counts). `move.n` = edge index:
  horizontal edges `0..(h+1)*w-1` then vertical edges after. Completing a box
  grants another turn.

### 3.2 Drawing + guessing (`draw`)

Host selects the game; the ESP runs short rounds from all non-empty streamed Draw packs,
rotating both the pack and the drawer. The per-pack word order is shuffled and does not
repeat until that pack is exhausted. Server -> client `t:"draw"`:
- `phase:"draw"`, `role:"drawer"`: `word`, `round`, `drawer` (pid), `scores`.
- `phase:"draw"`, `role:"guesser"`: `len` (word length), `round`, `drawer`
  (nick), `scores`.
- `phase:"reveal"`: `word`, `winner` (pid or null), `scores`.
- `phase:"idle"`: `scores`.

Ink: the drawer sends line segments `stroke{x0,y0,x1,y1}` (normalized finite 0..1) and
`clear{}`. All four coordinates must be present and valid or the whole stroke is dropped;
the server relays canonical numbers to guessers as `ink{x0,y0,x1,y1}` /
`ink{clear:true}`. Drawers advance fairly across replays, and non-empty content packs are
used round-robin (`draw.pack` names the active pack).
Guessing: a guesser sends `guess{text}`; a correct guess (case-insensitive) scores
and ends the round; a wrong guess is broadcast as `chat{nick,text}`.

### 3.3 Pong (`pong`)

1v1 via the same `challenge`/`accept`/`cancel`/`leaveGame` flow. Real-time: the
ESP ticks the ball + paddles and broadcasts. Server -> client `t:"pong"`:
`phase` ("lobby"/"playing"/"over"), `challenges` (lobby); playing: `you`, `me`
(1/2), `opp`, `ball{x,y}` (0..1), `p1`, `p2` (paddle y, 0..1), `s1`, `s2`
(scores); over: `result`. Client input: `paddle{dir}` with `dir` -1/0/1.

### 3.4 Host events

Protocol v18 replaces game-shaped result JSON with the fixed `EVENT` envelope in section
1.2. Kinds cover match start, chat, role assignment, round win/draw/completion, and game
final. Consumers format those fields against their own bounded roster mirror; unknown
kinds/versions are ignored safely.

### 3.5 Notes

- Only one game is active at a time (host-selected), so the duel lobby/challenge
  machinery is shared and parameterized by the active `kind`.
- `move` unifies to `{t:"move","n":<index>}` for every duel (connect4 included;
  it previously used `col`).

---

## 4. v0.2.0 — identity, reactions, four more games

New game ids (UART `SELECT_GAME` / lobby `game`): `7` react, `8` wyr, `9`
scramble, `10` reversi. Lobby `game` string adds `"react"`, `"wyr"`,
`"scramble"`, `"reversi"`. Firmware **v6** (`HA_FW_VERSION`).

### 4.1 Player identity + reactions

- `hello` gains an optional `avatar` field (an emoji, UTF-8, default 🙂). Every
  player object in `players`/leaderboard/podium messages now carries `avatar`.
- New client intent `react{emoji}`. The ESP broadcasts it to everyone as a
  **distinct** type `{"t":"emoji","pid","nick","avatar","emoji"}` (not `react`,
  which is the reaction-duel game state — see below).
- New client intent `say{text}` broadcasts a lobby/draw chat line as
  `{"t":"chat","nick","text"}` (the server echoes it back, so clients never render
  their own locally).

### 4.2 Reversi / Othello (`kind:"reversi"`)

A fourth duel on the shared challenge/accept/rematch flow. `cols:8`, `rows:8`,
`board` (64 ints, row-major, 0 empty / 1 / 2). `move.n` = cell 0..63; only cells
that flank and flip at least one opponent disc are legal. Extra fields: `sme`,
`sopp` (disc counts) and `valid` (array of legal cell indices for the player to
move, so the client can hint them). The ESP auto-passes a player with no legal
move and ends the game — most discs wins — when neither can move.

### 4.3 Whole-group party games

Three self-organizing games share a lobby -> countdown -> round -> reveal ->
final flow. Common client intents: `ready{ready:bool}` (ready-up in the lobby),
`again` (replay from the final screen). Common server phases: `"lobby"`
(`players:[{pid,nick,avatar,ready}]`), `"countdown"` (`sec`), and `"final"`.
Timed phases send relative `remaining_ms` and `duration_ms`, both in milliseconds.

- **Would You Rather** (`t:"wyr"`): `"vote"`/`"reveal"` carry `round`, `rounds`,
  `a`, `b` (the two options), `myvote` (0/1/-1), `counts` ([a,b]). Vote with the
  existing `answer{c:0|1}` intent. No scoring — it's a poll.
- **Word Scramble** (`t:"scramble"`): `"play"` carries `round`, `rounds`, `scram`
  (shuffled letters), `len`, `solved` (bool, you), `remaining_ms`, `duration_ms`, `scores`.
  Guess with the existing `guess{text}` intent; first correct scores most
  (200/120/80/40). `"reveal"` carries `word`; `"final"` a `board` podium.
- **Reaction Duel** (`t:"react"`): `"armed"` carries `round`, `rounds`, `light`
  ("wait"/"go"), `dq`, `tapped`, `scores`. Tap with the new `tap` intent; the
  first valid tap after `light:"go"` wins (200), tapping while `"wait"` DQs you
  for the round. `"reveal"` carries `winner` (nick or null), `ms`, `iwon`;
  `"final"` a `board` podium.

## 5. Guess the Color (`gc`) — game id `11`

Whole-group round game on the same `Party` skeleton (`lobby -> countdown -> play
-> reveal -> ... -> final`, 5 rounds). Select with UART `SELECT_GAME` id `11`;
lobby `game` string is `"gc"`. Firmware **v12**.

Client intents: `ready{ready:bool}` (lobby), `again` (from final), and
`guess{r,g,b}` (submit your color, each 0-255). The `guess` type is shared with
draw/scramble, which send `guess{text}`; the ESP routes by which fields are present.

Server `{t:"gc",phase,...}`:
- `"play"`: `round`, `rounds`, `color` (`"#RRGGBB"`, the target swatch to match —
  the numeric answer is hidden), `submitted` (bool, you), `scores`.
- `"reveal"`: `round`, `rounds`, `r`,`g`,`b` (the true color), `color`, `your`
  (`{r,g,b,color,dist,points}` or null if you didn't guess), `winner` (nick or
  null), `iwon`, `scores`.
- `"final"`: `board` (podium).

Scoring per round: `points = closeness + speed_bonus`, where
`closeness = round(200 * (1 - dist/441.67))` clamped ≥ 0 (`dist` = Euclidean RGB
distance) and `speed_bonus = round(100 * (1 - submit_ms/12000))` clamped ≥ 0.
The round winner is the highest points (ties broken by the faster submit).

## 6. Battleship (`bs`) — game id `12`

A 1v1 match game (like Pong): shares the challenge/lobby flow (`challenge`/`accept`/
`cancel`, `rematch`, `leaveGame`) but has its own state and screen. 10x10 grid, five ships
(5,4,3,3,2 = 17 cells). Select with UART `SELECT_GAME` id `12`; lobby `game` string `"bs"`.
Firmware **v13**.

Client intents (besides the shared match ones): `place{ships}` and `fire{n}`.
- `place{ships}`: `ships` is a string `"r,c,d;r,c,d;..."`, one triple per ship in fixed
  order (5,4,3,3,2); `d`=0 horizontal, `d`=1 vertical (anchor is the top/left cell). The
  server reconstructs the cells, validates bounds and no-overlap, stores the fleet, and
  marks the player ready. Invalid layouts get a `toast` and no ready. Both ready -> firing.
- `fire{n}` (0..99): a hit keeps your turn (shoot again); a miss passes it. Sinking a ship
  sends a `toast` to both players.

Server `{t:"bs",phase,...}`:
- `"place"`: `you`, `me` (1/2), `opp`, `ready`, `oppReady`. The client renders its own board.
- `"fire"`: `turn`, `yourTurn`, `opp`, `myShips`, `oppShips`, and two 100-cell arrays:
  `mine` (your fleet: 0 empty, 1 ship, 2 miss, 3 hit) and `track` (your shots on the enemy:
  0 un-shot, 1 miss, 2 hit, 3 hit+sunk).
- `"over"`: adds `result` (win/lose) and `oppFleet` (the enemy fleet revealed).

**Hidden information:** `track` is derived only from the shots you've fired, so an enemy
ship cell you haven't hit is never in the payload. `oppFleet` appears only in `"over"`.

## 7. Spectrum (`spectrum`) — game id `13`

A whole-group party game (Wavelength-style) on the shared party skeleton (lobby with a
ready-up + pack vote, countdown, reveal). Content reuses the pack pipeline: each item is a
`Left`/`Right` word pair. Select with UART `SELECT_GAME` id `13`; lobby `game` string
`"spectrum"`. Firmware **v14**.

Each round rotates a **psychic** who sees a hidden target on a 0-100 spectrum and types a
clue; everyone else slides a dial to guess. Points by closeness (±2 = 4, ±7 = 3, ±12 = 2),
and the psychic earns the guessers' average, so a good clue pays off. Six rounds.

Client intents: `ready`, `vote{pack}`, `clue{text}` (psychic only), `slide{n}` (0..100,
guessers only), `again`.

Server `{t:"spectrum",phase,...}`:
- `"lobby"`: `you`, `players`, `packs` (name/votes), `myvote`.
- `"countdown"`: `sec`.
- `"play"` with `stage` `"clue"` | `"guess"` | `"reveal"`: `round`, `rounds`, `left`,
  `right`, `psychic` (nick), `iam` (am I the psychic), `remaining_ms`/`duration_ms` for the timer bar.
  - `target` (0..100) is sent **only to the psychic** during clue/guess, and to everyone on
    reveal — an un-revealed target never reaches a guesser.
  - `clue` appears once the psychic has submitted; `myguess` is the guesser's own locked value.
  - reveal adds `guesses` (`nick`/`g`/`pts`) and `mygain`.
- `"final"`: `board` (the shared leaderboard).

## 8. Kiss Marry Kill (`kmk`) — game id `14`

A whole-group party game on the shared party skeleton (lobby with a ready-up + pack vote,
countdown, reveal). Content reuses the pack pipeline: each item is a `Name` (one person or
character). Select with UART `SELECT_GAME` id `14`; lobby `game` string `"kmk"`. Firmware
**v15**.

Each round rotates a **chooser** and draws three people from the pack. The chooser secretly
assigns Kiss / Marry / Kill; everyone else predicts that assignment. Points are the number
of matching positions (0, 1, or 3 — matching two forces the third), and the chooser earns
the guessers' average. Six rounds.

Client intents: `ready`, `vote{pack}`, `assign{kiss,marry,kill}` (each is the 0-2 index of
the person getting that label; the chooser sends it in the choose stage, guessers in the
guess stage), `again`.

Server `{t:"kmk",phase,...}`:
- `"lobby"`: `you`, `players`, `packs` (name/votes), `myvote`.
- `"countdown"`: `sec`.
- `"play"` with `stage` `"choose"` | `"guess"` | `"reveal"`: `round`, `rounds`, `chooser`
  (nick), `iam` (am I the chooser), `people` (the three names), `remaining_ms`/`duration_ms` for the timer.
  - `answer` (the chooser's Kiss/Marry/Kill labels) is sent **only to the chooser** from the
    guess stage on, and to everyone on reveal — a guesser never sees it early.
  - `mine` is the guesser's own submitted labels.
  - reveal adds `guesses` (`nick`/labels/`pts`) and `mygain`.
- `"final"`: `board` (the shared leaderboard).

## 9. Chess (`chess`) — game id `15`

A 1v1 duel (like Pong/Battleship): shares the challenge/lobby flow (`challenge`/`accept`/
`cancel`, `rematch`, `leaveGame`) but plays full FIDE rules, refereed entirely on the ESP.
Select with UART `SELECT_GAME` id `15`; lobby `game` string `"chess"`. Firmware **v18**.
Chess has no content packs — its UI strings are localized client-side from the message
catalog like every game (the host's `lang`, set via `CONFIG` and echoed in `welcome`); the
per-language `packs/<game>/<lang>/` streaming that content games use does not apply here.

Client intents (besides the shared match ones): `move{from,to[,promo]}`, `resign`, `draw`
(offer, or accept one already pending), `claim`.
- `move{from,to,promo}`: squares are 0-63, `a1 = 0` row-major (`h8 = 63`). `promo` is
  required exactly when the move lands a pawn on the last rank — `2` knight, `3` bishop,
  `4` rook, `5` queen — and must be omitted/0 otherwise; a mismatched, illegal, or
  malformed move is silently ignored.
- `resign`: forfeit the game immediately.
- `draw`: offers a draw if none is pending; if the opponent already offered, accepts it and
  ends the game as a draw. The opponent gets a `toast` when an offer arrives.
- `claim`: claims a draw when threefold repetition or the 50-move count currently stands
  for the player to move; a no-op otherwise.

Server `{t:"chess",phase,...}`:
- `"playing"`: `you`, `opp`, `white` (bool, are you playing white), `turn` (pid),
  `yourTurn`, `board` (64 chars, index 0 = a1, row-major to h8: `PNBRQK`/`pnbrqk`/`.`),
  `moves` (`[from*64+to, ...]`, your legal moves, always present but populated only for the player to move), `check`,
  `last` (`from*64+to` of the last move played, `-1` before the first), `remaining_ms`,
  `duration_ms`, `other_remaining_ms`, `wtm`, `claim3`, `claim50`, `offer` (`0` or the pid with a pending draw offer).
- `"over"`: the same fields minus `moves`/`claim3`/`claim50`, plus `result`
  (`"win"`/`"lose"`/`"draw"`) and `reason` (`mate`/`stalemate`/`resign`/`flag`/`flagdraw`/
  `material`/`rep3`/`rep5`/`move50`/`move75`/`agree`/`left`). Clock values are frozen
  relative snapshots in this phase. `offer` is also stale in this phase — it is not
  cleared when the game ends — so clients should ignore it once `phase` is `"over"`.
- `"lobby"`: `challenges` only.

Clocks: fixed 5+0 blitz, no increment, server-authoritative. `remaining_ms` is the time left
for the side to move (`wtm` = white to move); `other_remaining_ms` is the other side's
frozen time. The client locally subtracts elapsed time from the relative snapshot. A flag fall
loses the game for the side whose clock ran out, unless the opponent could not mate by any
legal sequence (FIDE 6.9), in which case it's a draw (`flagdraw`).

Draw rules: stalemate, dead position (insufficient material), fivefold repetition, and the
75-move rule end the game automatically; threefold repetition and the 50-move rule are
claimable only by the player to move, via `claim`, exactly while `claim3`/`claim50` reads
true; either player can also offer or accept a draw via `draw`. A pending offer lapses the
moment either side plays a move.

State is pushed only on events — a move, resign, draw, claim, or a flag fall the ESP
notices on its own clock tick — never on a periodic heartbeat; clients animate the
countdown locally between relative snapshots.
