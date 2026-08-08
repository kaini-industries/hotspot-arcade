// Hotspot Arcade game engine (ESP side, real-time referee).
// Owns the player roster and the authoritative live state for the active game.
// Header-only, included exactly once by the .ino (single translation unit), so
// it may define freely. It talks to the outside world only through the sink
// functions below, which the .ino implements (WS send, UART report).
#pragma once
#include <Arduino.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include "ha_json.h"
#include "ha_proto.h"

#ifndef HA_MAX_PLAYERS
#define HA_MAX_PLAYERS 12
#endif
#define HA_NICK_LEN 20
#define HA_RESUME_TOKEN_LEN 32
#define HA_IDENTITY_LEN (HA_IDENTITY_BYTES * 2)
#define HA_RESUME_GRACE_MS 120000UL

enum HaJoinAuthResult : uint8_t {
    HA_JOIN_AUTH_OK = 0,
    HA_JOIN_AUTH_REQUIRED = 1,
    HA_JOIN_AUTH_BAD_CODE = 2,
    HA_JOIN_AUTH_THROTTLED = 3,
    HA_JOIN_AUTH_KNOWN = 4, // host ledger recognizes the digest; code may be omitted
    HA_JOIN_AUTH_FULL = 5, // durable host ledger is full even if an engine pid is free
};

// Browser resume tokens are credentials and never become server state. Derive a
// stable 128-bit identity from SHA-256(token) and retain only its first 16 bytes,
// rendered as lowercase hex. The token is exactly one SHA-256 block after padding,
// so this fixed-input implementation avoids a general streaming hash context.
static void haIdentityDigest(const char token[HA_RESUME_TOKEN_LEN + 1],
                             char out[HA_IDENTITY_LEN + 1]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint8_t block[64] = {};
    memcpy(block, token, HA_RESUME_TOKEN_LEN);
    block[HA_RESUME_TOKEN_LEN] = 0x80;
    block[62] = 0x01; // 32 bytes = 256 bits, big-endian in the final eight bytes
    uint32_t w[64];
    for(int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    auto rotr = [](uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); };
    for(int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    static const char HEX_DIGEST[] = "0123456789abcdef";
    for(int i = 0; i < 16; i++) {
        uint8_t value = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
        out[i * 2] = HEX_DIGEST[value >> 4];
        out[i * 2 + 1] = HEX_DIGEST[value & 15];
    }
    out[HA_IDENTITY_LEN] = '\0';
}

// All game windows are far shorter than 2^31 ms. Signed-difference comparisons
// therefore remain correct when the ESP32's 32-bit millisecond clock rolls over.
static inline bool haTimeReached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}
static inline uint32_t haTimeRemaining(uint32_t now, uint32_t deadline) {
    int32_t remain = (int32_t)(deadline - now);
    return remain > 0 ? (uint32_t)remain : 0;
}

// Nicknames are uppercased once, here at the door, so every downstream consumer
// (phone UI, Flipper roster, and the strings this engine composes like "A vs B")
// is consistent without each one having to remember. ASCII only on purpose:
// bytes >= 0x80 are UTF-8 continuation/lead bytes and are left untouched, so an
// accented or emoji nickname survives intact.
static inline void ha_upper(char* s) {
    for(; s && *s; s++)
        if(*s >= 'a' && *s <= 'z') *s -= 32;
}

// Duels (connect4 / tic-tac-toe / dots-and-boxes) share one match + challenge
// system, parameterized by the active game's kind. Only one game is active at a
// time, so all live matches are the active kind.
#define DUEL_MAX_CELLS 64 // c4 = 7x6; ttt = 3x3; reversi = 8x8
#ifndef DUEL_MAX_MATCHES
#define DUEL_MAX_MATCHES 6
#endif
#ifndef DUEL_MAX_CHALLENGES
#define DUEL_MAX_CHALLENGES 16
#endif
#define DOTS_W 5 // boxes across
#define DOTS_H 5 // boxes down
#define DOTS_HEDGES ((DOTS_H + 1) * DOTS_W) // horizontal edges
#define DOTS_VEDGES (DOTS_H * (DOTS_W + 1)) // vertical edges
#define DOTS_BOXES (DOTS_W * DOTS_H)

// Battleship: 1v1 on a 10x10 grid, five ships (5+4+3+3+2 = 17 cells). Its own match
// struct (like Pong), not the shared DuelMatch board, because it needs two grids per
// player and hidden fleets.
#define BS_SIZE 10
#define BS_N (BS_SIZE * BS_SIZE) // 100
#define BS_SHIPS 5
#define BS_TOTAL 17 // sum of BS_LEN, the win threshold
#ifndef BATTLE_MAX
#define BATTLE_MAX 4 // concurrent matches
#endif

// Chess: 1v1, full FIDE rules refereed here. Squares are 0..63 with a1 = 0, b1 = 1 ...
// h8 = 63, so rank = sq >> 3 and file = sq & 7. A move is encoded as from * 64 + to.
#ifndef CHESS_MAX
#define CHESS_MAX 4 // concurrent matches
#endif
#define CH_HIST 154 // repetition ring: 1 + 150 halfmoves (75-move bound) + slack
#define CH_CLOCK_MS 300000UL // 5:00 per side, no increment
#define CH_MAX_MOVES 220 // legal-move buffer (theoretical max is 218)
// How a finished game ended (ChessMatch::reason).
#define CH_R_MATE 1
#define CH_R_STALEMATE 2
#define CH_R_RESIGN 3
#define CH_R_FLAG 4 // opponent's clock ran out
#define CH_R_FLAGDRAW 5 // clock ran out but the winner could not have mated (FIDE 6.9)
#define CH_R_MATERIAL 6 // dead position
#define CH_R_REP3 7 // threefold, claimed
#define CH_R_REP5 8 // fivefold, automatic
#define CH_R_MOVE50 9 // 50-move rule, claimed
#define CH_R_MOVE75 10 // 75-move rule, automatic
#define CH_R_AGREE 11 // draw by agreement
#define CH_R_LEFT 12 // opponent disconnected

#define TRIVIA_MAX_TOPICS 6
#define TRIVIA_MAX_QS 20
#define PACK_MAX_ITEMS 32 // items in a word/prompt pack (wyr/scramble/draw)
#define TRIVIA_QDUR 20 // seconds per question (safety timer)
#define TRIVIA_COUNTDOWN 3 // seconds after all-ready before the first question
#define TRIVIA_REVEAL_MS 4000 // pause on the reveal before the next question

// Character count of a UTF-8 string (counts lead bytes, skips continuation bytes).
// Content packs are UTF-8, so any language beyond ASCII (accented Latin, Cyrillic,
// Greek, ...) needs glyph-aware handling: a 2-byte letter must count as one blank in
// Draw, and Scramble must shuffle whole letters, not split them into invalid bytes.
static inline int haUtf8Len(const char* s) {
    int n = 0;
    for(; *s; s++)
        if(((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

#define DRAW_SECS 70 // per drawing round
#define DRAW_REVEAL_MS 4000 // reveal pause before the next round
#ifndef PONG_MAX
#define PONG_MAX 4 // concurrent pong matches
#endif
#define PONG_WIN 5 // points to win
#define PONG_TICK_MS 33 // ~30 Hz
// Court geometry, as fractions of the canvas width. The ball must reverse when its
// EDGE meets the paddle FACE, so the contact plane is paddle thickness + ball half
// width in from the wall. Bouncing at a bare 0.05 (as this did) left the ball
// visibly short of the paddle, because the paddle only reaches 0.02 and the ball's
// edge is 0.018 ahead of its centre. web/games/pong.js draws with these same two
// numbers — change one side and the ball bounces off empty space again.
#define PONG_PAD_W 0.02f // paddle thickness
#define PONG_BALL_R 0.018f // ball half width
#define PONG_HIT_X (PONG_PAD_W + PONG_BALL_R) // left contact plane; right is 1 - this

// Whole-group "party" games (would-you-rather / scramble / reaction) share a
// lobby -> countdown -> round -> reveal -> ... -> final skeleton (see Party).
#define PARTY_COUNTDOWN 3 // seconds after all-ready before round 1
#define WYR_ROUNDS 6
#define WYR_VOTE_SECS 20 // safety timer per prompt
#define WYR_REVEAL_MS 5000
#define SCR_ROUNDS 6
#define SCR_SECS 30 // safety timer per word
#define SCR_REVEAL_MS 5000
#define REACT_ROUNDS 5
#define REACT_REVEAL_MS 4000

// Spectrum (wavelength-style): each round one player is the psychic. They see a
// hidden target on a 0..100 spectrum between two opposing words and type a clue;
// everyone else slides to guess where the clue lands. Points by closeness; the
// psychic scores by how well the guessers do, so a good clue is rewarded.
#define SPECTRUM_ROUNDS 6
#define SPECTRUM_CLUE_SECS 45 // psychic's clue window (safety timer)
#define SPECTRUM_GUESS_SECS 30 // guessers' window (safety timer)
#define SPECTRUM_REVEAL_MS 6000
#define SPECTRUM_CLUE_LEN 40

// Kiss Marry Kill: each round one player (the "chooser") secretly assigns Kiss /
// Marry / Kill to three people from the voted pack; everyone else predicts that
// assignment. A guess of a permutation of three either matches 3 positions or at
// most 1 (getting two right forces the third), so per-round scores are 0/1/3.
#define KMK_ROUNDS 6
#define KMK_CHOOSE_SECS 40 // chooser's window (safety timer)
#define KMK_GUESS_SECS 30 // guessers' window (safety timer)
#define KMK_REVEAL_MS 7000

#define GC_ROUNDS 5
#define GC_PLAY_SECS 25 // safety deadline per color
#define GC_REVEAL_MS 6000
#define GC_SPEED_MS 12000 // speed bonus decays to 0 over this window

// ---- sinks implemented in the .ino ----
void haWsSendWs(uint32_t wsId, const String& msg); // to one socket (0 = no-op)
void haWsCloseWs(uint32_t wsId); // duplicate-identity takeover; newest socket wins
void haWsBroadcast(const String& msg); // to all connected sockets
uint8_t haAuthorizeIdentity(
    uint32_t wsId, const char* identity, const char* code, uint32_t* retryMs);
void haUartJoinStable(uint8_t pid, const char* identity, const char* nick, const char* avatar);
void haUartLeave(uint8_t pid);
void haUartScore(uint8_t pid, int delta, const char* reason);
void haUartHostEvent(uint8_t kind, uint8_t game, uint8_t actor, uint8_t target,
                     int16_t value, const char* text);

struct Player {
    bool used;
    uint32_t wsId; // 0 = not connected
    char identity[HA_IDENTITY_LEN + 1]; // SHA-256-derived identity; raw token stays browser-only
    bool detached;
    uint32_t detachedAt; // raw host millis, intentionally independent of the paused game clock
    char nick[HA_NICK_LEN];
    char avatar[8]; // emoji avatar (UTF-8), player-picked on the landing screen
    int32_t score;
};

// Trivia content, streamed from the Flipper at session start (the packs become
// the votable topics), then owned by the ESP which orchestrates the whole game.
struct TriviaQ {
    String q;
    String o[4];
    uint8_t correct;
};
struct TriviaTopic {
    String name;
    TriviaQ qs[TRIVIA_MAX_QS];
    uint8_t qcount;
};

struct Trivia {
    uint8_t phase; // 0 lobby, 1 countdown, 2 question, 3 reveal, 4 final
    bool ready[HA_MAX_PLAYERS + 1];
    int8_t vote[HA_MAX_PLAYERS + 1]; // topic index, -1 = none
    uint32_t countdownEnd;
    int lastSec; // last countdown second broadcast
    uint8_t topic; // chosen topic index
    int qi; // current question index
    int8_t answer[HA_MAX_PLAYERS + 1];
    uint32_t answerMs[HA_MAX_PLAYERS + 1];
    int gained[HA_MAX_PLAYERS + 1]; // points earned on the current question
    int counts[4];
    uint32_t deadline; // question end
    uint32_t revealUntil;
};

struct DuelMatch {
    bool used;
    uint8_t kind; // HA_GAME_CONNECT4 / TICTACTOE / DOTS
    uint8_t a, b; // pids; a plays mark 1, b plays mark 2
    bool aIn, bIn; // still attached (not returned to lobby)
    uint8_t turn; // pid to move
    uint8_t phase; // 1 playing, 2 over
    uint8_t winner; // pid, or 0 for draw
    uint8_t first; // who moved first (rematch alternates it)
    uint8_t board[DUEL_MAX_CELLS]; // grid games (c4/ttt), row-major; 0/1/2
    uint8_t hedges[DOTS_HEDGES]; // dots: horizontal edges drawn (0/1)
    uint8_t vedges[DOTS_VEDGES]; // dots: vertical edges drawn (0/1)
    uint8_t boxes[DOTS_BOXES]; // dots: box owner (0/1/2)
    uint8_t sA, sB; // dots: box counts for a / b
};

struct DuelChallenge {
    bool used;
    uint16_t id;
    uint8_t game;
    uint8_t from, to;
};

// Shared word pack for scramble/draw: a set of single-word items, voted on like
// trivia topics / wyr packs. Mirrors WyrPack but with one word per item.
struct WordPack {
    String name;
    String words[PACK_MAX_ITEMS];
    uint8_t count;
};

struct DrawState {
    uint8_t phase; // 0 idle, 1 draw, 2 reveal, 3 final
    uint8_t drawer; // pid currently drawing
    uint8_t lastDrawer; // retained across games so a capped run never starves the same players
    uint8_t nextPack; // retained across games; each round advances to another non-empty pack
    uint16_t wordSeq[TRIVIA_MAX_TOPICS]; // per-pack cursors, reset only with content
    uint8_t wordOrder[TRIVIA_MAX_TOPICS][PACK_MAX_ITEMS]; // shuffled nonrepeating decks
    uint8_t lastWord[TRIVIA_MAX_TOPICS]; // prevents a repeat across reshuffle boundaries
    char word[24];
    int round;
    int roundsTotal; // game ends after this many rounds
    uint32_t deadline; // millis (draw end)
    uint32_t revealUntil; // millis (reveal end)
    uint8_t winner; // pid who guessed it, or 0
    WordPack* packs; // lazily allocated; pack cap mirrors trivia's topic cap
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted (no vote strip yet; see Task 3)
    uint8_t pack; // chosen pack index (pack 0 for now, no draw vote strip)
};

// Shared lobby/ready/countdown skeleton for the whole-group party games.
// phase: 0 lobby, 1 countdown, 2 round, 3 reveal, 4 final.
struct Party {
    uint8_t phase;
    bool ready[HA_MAX_PLAYERS + 1];
    int round; // 1-based current round
    int roundsTotal;
    uint32_t countdownEnd;
    int lastSec; // last countdown second broadcast
    uint32_t deadline; // round safety deadline
    uint32_t revealUntil; // reveal end
};

// Would You Rather: a live A/B poll. Prompts come from the voted pack.
struct WyrPrompt {
    String a, b;
};
struct WyrPack {
    String name;
    WyrPrompt items[PACK_MAX_ITEMS];
    uint8_t count;
};
struct WyrState {
    Party pt;
    WyrPack* packs; // lazily allocated; cap mirrors HA_MAX_TOPICS on the Flipper side
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted
    uint8_t pack; // chosen pack index (locked in when the round starts)
    uint8_t promptSeq; // rotates prompts across rounds within the pack
    uint8_t prompt; // current prompt index within the chosen pack
    int8_t choice[HA_MAX_PLAYERS + 1]; // A/B vote for the current prompt, -1 = none
};

// Word scramble race: everyone unscrambles the same word; fastest correct win most.
struct ScrambleState {
    Party pt;
    uint16_t wordSeq;
    char word[24]; // the answer
    char scram[24]; // shown (letters shuffled)
    bool solved[HA_MAX_PLAYERS + 1];
    uint8_t solvedCount;
    WordPack* packs; // lazily allocated; pack cap mirrors trivia's topic cap
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted
    uint8_t pack; // chosen pack index (locked in when the round starts)
};

// Reaction duel (fastest finger): red -> (random delay) -> green; first tap wins.
// Tapping while red disqualifies you for the round.
struct ReactState {
    Party pt;
    uint32_t goAt; // millis the light turns green (phase 2)
    bool goOn; // green announced this round
    bool tapped[HA_MAX_PLAYERS + 1];
    bool dq[HA_MAX_PLAYERS + 1]; // false-started this round
    uint8_t winner; // pid, or 0
    uint32_t winMs; // winner's reaction time
};

// Guess the Color: a random swatch is shown; everyone dials in an R/G/B guess and
// submits. Points = closeness (Euclidean RGB distance) + a speed bonus that decays
// the longer you take. Closest usually wins the round; a fast submit can edge it.
struct GuessColorState {
    Party pt;
    uint8_t tr, tg, tb; // target color for the round
    uint32_t roundStart; // millis the play phase began (for speed)
    bool guessed[HA_MAX_PLAYERS + 1];
    uint8_t gr[HA_MAX_PLAYERS + 1], gg[HA_MAX_PLAYERS + 1], gb[HA_MAX_PLAYERS + 1];
    uint32_t submitMs[HA_MAX_PLAYERS + 1]; // reveal -> submit, ms
    int gained[HA_MAX_PLAYERS + 1]; // points earned this round
    uint8_t winner; // pid with the most points this round, 0 = none
};

// Spectrum: reuses WyrPack for content (each item's a=left label, b=right label)
// and the Party lobby/countdown/reveal skeleton. Within a playing round it has two
// stages: 0 = the psychic is writing the clue, 1 = everyone else is guessing.
struct SpectrumState {
    Party pt;
    WyrPack* packs; // lazily allocated; item.a = left word, item.b = right word
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted
    uint8_t pack; // chosen pack (locked when the game starts)
    uint16_t cardSeq; // rotates the spectrum card across rounds
    uint8_t card; // current card index within the pack
    uint8_t psychic; // pid giving the clue this round
    uint8_t psychicSeq; // rotates the psychic across rounds
    uint8_t stage; // 0 clue, 1 guess
    int target; // hidden target 0..100
    char clue[SPECTRUM_CLUE_LEN]; // psychic's clue text
    int8_t guess[HA_MAX_PLAYERS + 1]; // 0..100, -1 = not guessed
    int gained[HA_MAX_PLAYERS + 1]; // points earned this round (shown on reveal)
};

// Kiss Marry Kill: reuses WordPack (a flat list of names) and the Party skeleton.
// Labels are 0 = kiss, 1 = marry, 2 = kill; each round has three people and the
// assignment is a permutation of those three labels over them.
struct KmkState {
    Party pt;
    WordPack* packs; // lazily allocated; each item is one person's name
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted
    uint8_t pack; // chosen pack (locked when the game starts)
    uint16_t nameSeq; // advances the people picked across rounds
    uint8_t person[3]; // indices into the pack for this round's three people
    uint8_t chooser; // pid assigning K/M/K this round
    uint8_t chooserSeq; // rotates the chooser across rounds
    uint8_t stage; // 0 choose, 1 guess
    int8_t cLabel[3]; // chooser's label per person, -1 = unset
    int8_t gLabel[HA_MAX_PLAYERS + 1][3]; // each guesser's labels per person
    bool guessed[HA_MAX_PLAYERS + 1];
    int gained[HA_MAX_PLAYERS + 1]; // points earned this round (shown on reveal)
};

struct PongMatch {
    bool used;
    uint8_t a, b; // a = left paddle, b = right paddle
    bool aIn, bIn;
    uint8_t phase; // 1 playing, 2 over
    float bx, by, vx, vy; // ball position (0..1) + velocity per tick
    float p1, p2; // paddle centers (0..1)
    int8_t d1, d2; // paddle move dir (-1/0/1)
    uint8_t s1, s2; // scores
    uint8_t winner; // pid
};

// Battleship: a = challenger, b = opponent. Each keeps a hidden fleet grid; shots are
// recorded on the *target's* grid. battleJson never exposes an un-hit enemy ship cell.
struct BattleMatch {
    bool used;
    uint8_t a, b; // pids
    bool aIn, bIn;
    uint8_t phase; // 0 placement, 1 firing, 2 over
    uint8_t turn; // pid to fire (firing phase)
    uint8_t first; // who fired first (rematch alternates it)
    uint8_t winner; // pid, or 0
    bool readyA, readyB; // placement committed
    uint8_t fleetA[BS_N], fleetB[BS_N]; // 0 empty, else ship id 1..BS_SHIPS
    uint8_t shotOnA[BS_N], shotOnB[BS_N]; // shots landed on that grid: 0 none, 1 miss, 2 hit
    uint8_t hitsA, hitsB; // hits scored BY a / BY b (win at BS_TOTAL)
};

static const uint8_t BS_LEN[BS_SHIPS] = {5, 4, 3, 3, 2};
static const char* const BS_NAMES[BS_SHIPS] = {
    "Carrier", "Battleship", "Cruiser", "Submarine", "Destroyer"};

// ---- chess tables and piece helpers ----
// Step tables are (file, rank) deltas rather than square offsets, so every step is
// bounds-checked on both axes: a raw +-1 on the square index wraps around the board
// edge (h4 + 1 lands on a5) and would invent moves that do not exist.
static const int8_t CH_NDF[8] = {1, 2, 2, 1, -1, -2, -2, -1}; // knight
static const int8_t CH_NDR[8] = {2, 1, -1, -2, -2, -1, 1, 2};
static const int8_t CH_KDF[8] = {-1, 0, 1, -1, 1, -1, 0, 1}; // king (and the 8 neighbours)
static const int8_t CH_KDR[8] = {1, 1, 1, 0, 0, -1, -1, -1};
static const int8_t CH_SDF[8] = {1, 1, -1, -1, 1, -1, 0, 0}; // sliders: 0..3 diagonal,
static const int8_t CH_SDR[8] = {1, -1, 1, -1, 0, 0, 1, -1}; // 4..7 orthogonal

// Piece codes in ChessCore::sq are 0 empty, 1..6 white P,N,B,R,Q,K, 7..12 black.
static inline uint8_t chKind(uint8_t pc) { return pc > 6 ? (uint8_t)(pc - 6) : pc; }
static inline bool chIsWhite(uint8_t pc) { return pc >= 1 && pc <= 6; }
static inline uint8_t chSide(uint8_t pc) { return pc > 6 ? 1 : 0; } // callers check pc != 0

// Castling right lost when a corner square changes hands (the rook moving off it, or
// being captured on it). 0 for every other square.
static inline uint8_t chCornerBit(int sq) {
    return sq == 0 ? 0x02 : sq == 7 ? 0x01 : sq == 56 ? 0x08 : sq == 63 ? 0x04 : 0;
}

// Zobrist keys: 12*64 piece-square, one side-to-move, 16 castling-rights states, 8
// en-passant files. Derive any indexed key directly from a fixed seed: this keeps
// hashes deterministic without a 3,172-byte mutable table in DRAM.
static inline uint32_t chessZobKey(unsigned index) {
    uint32_t z = 0x9E3779B9UL * (uint32_t)(index + 2);
    z = (z ^ (z >> 16)) * 0x85EBCA6BUL;
    z = (z ^ (z >> 13)) * 0xC2B2AE35UL;
    return z ^ (z >> 16);
}

// The position identity per FIDE 9.2: placement, side to move, castling rights and
// en-passant capturability. Everything repetition hashing has to cover, and nothing
// else: the clocks and move counters live in ChessMatch.
struct ChessCore {
    uint8_t sq[64];
    uint8_t stm; // 0 white, 1 black
    uint8_t rights; // castling: 1 = white O-O, 2 = white O-O-O, 4 = black O-O, 8 = black O-O-O
    int8_t ep; // en-passant target (the square the pawn skipped), -1 none
};

// What chessMake has to hand back so chessUnmake can restore the position exactly.
// capSq differs from the move's `to` only for an en-passant capture.
struct ChessUndo {
    uint8_t captured, capSq, rights;
    int8_t ep;
};

// Chess: a = challenger, b = opponent. `white` is a pid rather than a flag because a
// rematch swaps colors. hist[] is the repetition record: every position since the
// last irreversible move (pawn move or capture), which is what bounds it to CH_HIST.
struct ChessMatch {
    bool used;
    uint8_t a, b; // pids
    bool aIn, bIn;
    uint8_t white; // pid playing white this game
    uint8_t phase; // 1 playing, 2 over
    uint8_t winner; // pid, 0 = draw
    uint8_t reason; // CH_R_*
    ChessCore core;
    uint8_t halfmove; // plies since a pawn move/capture; 100 = 50-move claim, 150 = auto
    uint16_t fullmove;
    uint32_t clockMs[2]; // remaining ms, [0] = white, [1] = black
    uint32_t lastStamp; // clockNow() at game start / last completed move
    bool clockPaused; // one participant is inside the transient reconnect grace
    int16_t lastMove; // from * 64 + to of the move just played, -1 before the first
    uint8_t offerBy; // pid with a pending draw offer, 0 none
    uint16_t histLen;
    uint32_t hist[CH_HIST];
};

// Exactly one host-selected game is active, so its 1v1 match slots may share
// storage. Content/lobby state stays independent because every pack is streamed
// up front, but retaining four mutually exclusive match tables wastes scarce DRAM.
union MatchPools {
    DuelMatch duel[DUEL_MAX_MATCHES];
    PongMatch pong[PONG_MAX];
    BattleMatch battle[BATTLE_MAX];
    ChessMatch chess[CHESS_MAX];
};

class Engine {
public:
    // The phone-client UI language, set by the host and echoed to each phone in `welcome`
    // so the client loads the matching message catalog. "" = English. Content packs are a
    // separate, Flipper-side concern (which packs get streamed).
    void setLang(const char* l) {
        char next[sizeof(_lang)];
        strlcpy(next, (l && l[0]) ? l : "", sizeof(next));
        // CONFIG sent between CONTENT_CLEAR and CONTENT_COMMIT belongs to that
        // transaction.  Do not expose a new locale with the old content bank.
        if(_contentTransactionOpen) {
            strlcpy(_pendingLang, next, sizeof(_pendingLang));
            _pendingLangValid = true;
            return;
        }
        if(strcmp(next, _lang) == 0) return;
        strlcpy(_lang, next, sizeof(_lang));
        broadcastConfig();
    }

    void broadcastConfig() {
        haWsBroadcast(String("{\"t\":\"config\",\"lang\":\"") +
                      ha_json_escape(_lang) + "\"}");
    }

    void reset(uint32_t rawNow = 0) {
        discardContentStage();
        _clockNow = 0;
        _lastRawNow = rawNow;
        _clockStarted = true;
        _clockPaused = false;
        _pauseActive = false;
        _pauseReason[0] = '\0';
        _pauseSsid[0] = '\0';
        _pauseReconnectMs = 0;
        _criticalPausePid = 0;
        _nextChallengeId = 1;
        makeSessionId(_session);
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) _p[i] = Player{};
        _active = HA_GAME_NONE;
        triviaClear();
        duelClear();
        drawClear();
        pongClear();
        wyrClear();
        scrambleClear();
        reactClear();
        gcClear();
        battleClear();
        spectrumClear();
        kmkClear();
        chessClear();
        drawResetRotation();
    }

    // Freeze only game time. Raw time keeps advancing so reconnect grace remains
    // measurable; normal resume grants each detached player a fresh grace window,
    // while an expired host reconnect window finalizes them before unpausing.
    void transportPause(uint32_t rawNow) {
        advanceClock(rawNow);
        if(!_pauseActive) announceServerPause("ap_off", "", 600000);
        _clockPaused = true;
    }
    void transportResume(uint32_t rawNow, bool expireDetachedNow = false) {
        // START/transportResume may be delivered more than once.  A redundant
        // resume must be a complete no-op: in particular, it must not renew a
        // normally disconnected player's 120-second grace window.
        if(!_clockPaused) return;
        // Establish the new raw-time baseline while logical time is still frozen.
        // A host-expired planned reconnect window must finalize missing seats here,
        // rather than unpausing and accidentally granting each one another 120s.
        _lastRawNow = rawNow;
        _clockStarted = true;
        if(expireDetachedNow) {
            for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
                if(_p[pid].used && _p[pid].detached) finalizeLeave(pid);
        }
        _clockPaused = false;
        if(!expireDetachedNow) {
            for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
                if(_p[pid].used && _p[pid].detached) _p[pid].detachedAt = rawNow;
        }
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(_p[pid].used && _p[pid].wsId)
                resumeAffectedGame(pid, rawNow);
            else if(_p[pid].used) {
                pauseAffectedGame(pid, rawNow);
                duelRemoveChallengesInvolving(pid);
            }
        }
        triviaOnRosterChange();
        partyRosterChanged();
        haWsBroadcast("{\"t\":\"server_resume\"}");
        _pauseActive = false;
        _pauseReason[0] = '\0';
        _pauseSsid[0] = '\0';
        _pauseReconnectMs = 0;
        pushAll();
    }

    void announceServerPause(const char* reason, const char* ssid, uint32_t reconnectMs) {
        strlcpy(_pauseReason, reason && reason[0] ? reason : "ap_off", sizeof(_pauseReason));
        strlcpy(_pauseSsid, ssid ? ssid : "", sizeof(_pauseSsid));
        _pauseReconnectMs = reconnectMs;
        _pauseActive = true;
        haWsBroadcast(serverPauseJson());
    }

    // ---- roster ----
    uint8_t pidByWs(uint32_t wsId) {
        if(!wsId) return 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used && _p[i].wsId == wsId) return i;
        return 0;
    }

    void onWsDisconnect(uint32_t wsId, uint32_t rawNow) {
        advanceClock(rawNow);
        uint8_t pid = pidByWs(wsId);
        if(!pid) return;
        _p[pid].wsId = 0;
        _p[pid].detached = true;
        _p[pid].detachedAt = rawNow;
        if(_clockPaused) return; // planned teardown cannot alter quorum/matches/challenges
        pauseAffectedGame(pid, rawNow);
        // A challenge is not a live match and must not survive either endpoint
        // disappearing. Live matches remain reserved until reconnect/expiry.
        duelRemoveChallengesInvolving(pid);
        triviaOnRosterChange();
        partyRosterChanged();
        pushAll();
    }

    void onHello(uint32_t wsId, const char* nick, const char* avatar,
                 const char* resume, const char* code, uint32_t rawNow) {
        // A hello can arrive even when the loop task has not ticked recently.
        // Enforce the exact grace boundary here before looking up the identity,
        // otherwise a 120s+ socket could revive stale game state.
        if(!_clockPaused && expireDetached(rawNow)) pushAll();
        if(!validResumeToken(resume)) {
            haWsSendWs(wsId, "{\"t\":\"reject\",\"code\":\"bad_protocol\",\"proto\":2}");
            return;
        }
        char identity[HA_IDENTITY_LEN + 1];
        haIdentityDigest(resume, identity);
        uint8_t pid = pidByWs(wsId);
        bool resumed = false;
        if(pid && strcmp(_p[pid].identity, identity) != 0) {
            haWsSendWs(wsId, "{\"t\":\"reject\",\"code\":\"bad_protocol\",\"proto\":2}");
            return;
        }
        if(!pid) {
            pid = pidByIdentity(identity);
            resumed = pid != 0;
        }
        if(!pid) {
            uint32_t retryMs = 0;
            uint8_t auth = haAuthorizeIdentity(wsId, identity, code ? code : "", &retryMs);
            if(auth != HA_JOIN_AUTH_OK && auth != HA_JOIN_AUTH_KNOWN) {
                const char* reason = auth == HA_JOIN_AUTH_REQUIRED ? "auth_required" :
                                     auth == HA_JOIN_AUTH_THROTTLED ? "throttled" :
                                     auth == HA_JOIN_AUTH_FULL ? "full" : "bad_code";
                String reject = String("{\"t\":\"reject\",\"code\":\"") + reason + "\"";
                if(auth == HA_JOIN_AUTH_THROTTLED) reject += String(",\"retry_ms\":") + retryMs;
                reject += "}";
                haWsSendWs(wsId, reject);
                return;
            }
            // A restored host-ledger identity may reclaim a fresh engine seat
            // during the post-reboot reconnect window. A genuinely new party
            // admission waits for explicit host resume so the pre-pause roster
            // cannot change underneath a suspended game.
            if(_clockPaused && auth != HA_JOIN_AUTH_KNOWN) {
                haWsSendWs(wsId, _pauseActive ? serverPauseJson() :
                           String("{\"t\":\"server_pause\",\"reason\":\"ap_off\",\"ssid\":\"\",\"reconnect_ms\":600000}"));
                return;
            }
            pid = freePid();
            if(!pid) {
                haWsSendWs(wsId, "{\"t\":\"reject\",\"code\":\"full\"}");
                return;
            }
            clearPidState(pid);
            _p[pid].used = true;
            _p[pid].wsId = wsId;
            strlcpy(_p[pid].identity, identity, sizeof(_p[pid].identity));
            _p[pid].score = 0;
            strlcpy(_p[pid].nick, (nick && nick[0]) ? nick : "PLAYER", HA_NICK_LEN);
            ha_upper(_p[pid].nick);
            strlcpy(_p[pid].avatar, (avatar && avatar[0]) ? avatar : "\xF0\x9F\x99\x82", sizeof(_p[pid].avatar));
        } else {
            uint32_t oldWs = _p[pid].wsId;
            _p[pid].wsId = wsId; // newest socket wins; old socket no longer resolves to this pid
            _p[pid].detached = false;
            if(oldWs && oldWs != wsId) haWsCloseWs(oldWs);
            // Re-hello from a known socket = the player changed their name/avatar in
            // the header editor. Re-announce over UART so the Flipper's leaderboard
            // updates (player_join there updates an existing pid's nick in place).
            if(nick && nick[0]) {
                strlcpy(_p[pid].nick, nick, HA_NICK_LEN);
                ha_upper(_p[pid].nick);
            }
            if(avatar && avatar[0]) strlcpy(_p[pid].avatar, avatar, sizeof(_p[pid].avatar));
        }
        _p[pid].detached = false;
        if(!_clockPaused) resumeAffectedGame(pid, rawNow);
        haUartJoinStable(pid, _p[pid].identity, _p[pid].nick, _p[pid].avatar);
        String w = String("{\"t\":\"welcome\",\"proto\":2,\"session\":\"") + _session +
                   "\",\"pid\":" + pid +
                   ",\"nick\":\"" + ha_json_escape(_p[pid].nick) + "\",\"avatar\":\"" +
                   ha_json_escape(_p[pid].avatar) + "\",\"lang\":\"" +
                   ha_json_escape(_lang) + "\",\"resumed\":" + (resumed ? "true" : "false") +
                   ",\"paused\":" + (_clockPaused ? "true" : "false") + "}";
        haWsSendWs(wsId, w);
        if(!_clockPaused) {
            triviaOnRosterChange();
            partyRosterChanged();
        }
        pushAll();
        if(_clockPaused && _pauseActive) haWsSendWs(wsId, serverPauseJson());
    }

    // ---- host (Flipper) driven ----
    void selectGame(uint8_t id) {
        _criticalPausePid = 0;
        _active = id;
        triviaClear();
        duelClear();
        drawClear();
        pongClear();
        wyrClear();
        scrambleClear();
        reactClear();
        gcClear();
        battleClear();
        spectrumClear();
        kmkClear();
        chessClear();
        pushAll();
    }

    void resetScores() {
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used) _p[i].score = 0;
        pushAll();
    }

    // ---- trivia content streamed from the Flipper (packs -> votable topics) ----
    void triviaTopicsClear() {
        delete[] _topics;
        _topics = nullptr;
        _topicCount = 0;
    }
    void triviaAddTopic(const char* name) {
        if(_topicCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_topics)) return;
        _topics[_topicCount] = TriviaTopic{};
        _topics[_topicCount].name = name;
        _topics[_topicCount].qcount = 0;
        _topicCount++;
    }
    void triviaAddQ(const char* json) {
        if(_topicCount == 0) return;
        TriviaTopic& tp = _topics[_topicCount - 1];
        if(tp.qcount >= TRIVIA_MAX_QS) return;
        TriviaQ& q = tp.qs[tp.qcount];
        char buf[200];
        q.q = ha_json_str(json, "q", buf, sizeof(buf)) ? buf : "";
        String opts[4];
        parseOptions(json, opts);
        for(int k = 0; k < 4; k++) q.o[k] = opts[k];
        int v;
        q.correct = ha_json_int(json, "c", &v) ? (uint8_t)v : 0;
        tp.qcount++;
    }

    // ---- generic content ingest ------------------------------------------------
    // The Flipper streams packs it does not understand: "Key: value" blocks, shipped
    // as JSON objects of the file's own keys. All game semantics live here, so adding
    // a content game needs a loader below and nothing on the Flipper.
    void clearContentStorage() {
        triviaTopicsClear();
        delete[] _wyr.packs;
        _wyr.packs = nullptr;
        _wyr.packCount = 0;
        delete[] _scr.packs;
        _scr.packs = nullptr;
        _scr.packCount = 0;
        delete[] _d.packs;
        _d.packs = nullptr;
        _d.packCount = 0;
        drawResetRotation();
        delete[] _spec.packs;
        _spec.packs = nullptr;
        _spec.packCount = 0;
        delete[] _kmk.packs;
        _kmk.packs = nullptr;
        _kmk.packCount = 0;
        _packGame = 0;
    }

    bool contentPackDirect(uint8_t game, const char* name) {
        _packGame = 0;
        if(game == HA_GAME_TRIVIA) {
            uint8_t before = _topicCount;
            triviaAddTopic(name);
            if(_topicCount == before) return false;
        } else if(game == HA_GAME_WYR) {
            if(_wyr.packCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_wyr.packs)) return false;
            _wyr.packs[_wyr.packCount] = WyrPack{};
            _wyr.packs[_wyr.packCount++].name = name;
        } else if(game == HA_GAME_SCRAMBLE) {
            if(_scr.packCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_scr.packs)) return false;
            _scr.packs[_scr.packCount] = WordPack{};
            _scr.packs[_scr.packCount++].name = name;
        } else if(game == HA_GAME_DRAW) {
            if(_d.packCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_d.packs)) return false;
            _d.packs[_d.packCount] = WordPack{};
            _d.packs[_d.packCount++].name = name;
        } else if(game == HA_GAME_SPECTRUM) {
            if(_spec.packCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_spec.packs)) return false;
            _spec.packs[_spec.packCount] = WyrPack{};
            _spec.packs[_spec.packCount++].name = name;
        } else if(game == HA_GAME_KMK) {
            if(_kmk.packCount >= TRIVIA_MAX_TOPICS || !ensureContentStorage(_kmk.packs)) return false;
            _kmk.packs[_kmk.packCount] = WordPack{};
            _kmk.packs[_kmk.packCount++].name = name;
        } else {
            return true; // forward-compatible no-op for a newer host's unknown game id
        }
        _packGame = game;
        return true;
    }

    bool contentItemDirect(const char* json) {
        if(!_packGame) return true; // item for a forward-compatible unknown game
        if(_packGame == HA_GAME_TRIVIA) return triviaLoadItem(json);
        if(_packGame == HA_GAME_WYR) return wyrLoadItem(json);
        if(_packGame == HA_GAME_SCRAMBLE) return scrambleLoadItem(json);
        if(_packGame == HA_GAME_DRAW) return drawLoadItem(json);
        if(_packGame == HA_GAME_SPECTRUM) return spectrumLoadItem(json);
        if(_packGame == HA_GAME_KMK) return kmkLoadItem(json);
        // Unknown game ids are dropped on purpose: a newer Flipper must not be able
        // to corrupt an older board's state.
        return true;
    }

    // Content replacement is a two-phase transaction. The old packs remain live
    // while a complete replacement is streamed into a heap-backed staging engine.
    void contentClear() {
        discardContentStage();
        _contentTransactionOpen = true;
        _pendingLangValid = false;
        _contentStagePacks = 0;
        _contentStageItems = 0;
        _contentStage = new(std::nothrow) Engine();
        if(_contentStage) _contentStage->drawResetRotation();
    }

    void contentPack(uint8_t game, const char* name) {
        if(_contentStage && _contentStage->contentPackDirect(game, name)) _contentStagePacks++;
    }

    void contentItem(const char* json) {
        if(_contentStage && ha_json_flat_object_valid(json) &&
           _contentStage->contentItemDirect(json)) _contentStageItems++;
    }

    void contentAbort() { discardContentStage(); }

#ifdef HA_ENGINE_TEST
    // Deterministically model staging allocation failure in the host simulator.
    // The transaction remains open so locale changes must stay staged and abort.
    void contentTestLoseStage() {
        if(_contentStage) {
            _contentStage->clearContentStorage();
            delete _contentStage;
        }
        _contentStage = nullptr;
        _contentStagePacks = 0;
        _contentStageItems = 0;
    }
#endif

    bool contentCommit(uint16_t expectedPacks = 0xFFFF, uint16_t expectedItems = 0xFFFF) {
        if(!_contentStage || (expectedPacks != 0xFFFF && expectedPacks != _contentStagePacks) ||
           (expectedItems != 0xFFFF && expectedItems != _contentStageItems)) {
            discardContentStage();
            return false;
        }
        int32_t scores[HA_MAX_PLAYERS + 1];
        for(uint8_t i = 0; i <= HA_MAX_PLAYERS; i++) scores[i] = _p[i].score;
        _criticalPausePid = 0;
        clearActiveRound();
        clearContentStorage();

        _topics = _contentStage->_topics;
        _topicCount = _contentStage->_topicCount;
        _contentStage->_topics = nullptr;
        _contentStage->_topicCount = 0;
#define HA_MOVE_PACKS(state)                                      \
        state.packs = _contentStage->state.packs;                 \
        state.packCount = _contentStage->state.packCount;         \
        _contentStage->state.packs = nullptr;                     \
        _contentStage->state.packCount = 0
        HA_MOVE_PACKS(_wyr);
        HA_MOVE_PACKS(_scr);
        HA_MOVE_PACKS(_d);
        HA_MOVE_PACKS(_spec);
        HA_MOVE_PACKS(_kmk);
#undef HA_MOVE_PACKS
        delete _contentStage;
        _contentStage = nullptr;
        _contentTransactionOpen = false;
        _contentStagePacks = 0;
        _contentStageItems = 0;
        _packGame = 0;
        drawResetRotation();
        for(uint8_t i = 0; i <= HA_MAX_PLAYERS; i++) _p[i].score = scores[i];
        if(_pendingLangValid) strlcpy(_lang, _pendingLang, sizeof(_lang));
        _pendingLangValid = false;
        broadcastConfig();
        pushAll();
        return true;
    }

    // Map a pack file's keys into TriviaQ. The file says {q,a,b,c,d,answer}; the
    // struct wants {q, o[4], correct}. Note "c" means option C here and the correct
    // INDEX in the struct — consuming this object raw would silently mark the wrong
    // answer, so every field is mapped explicitly.
    bool triviaLoadItem(const char* json) {
        if(_topicCount == 0) return false;
        TriviaTopic& tp = _topics[_topicCount - 1];
        if(tp.qcount >= TRIVIA_MAX_QS) return false;

        char buf[200];
        if(!ha_json_str(json, "q", buf, sizeof(buf))) return false;
        TriviaQ q;
        q.q = buf;

        static const char* keys[4] = {"a", "b", "c", "d"};
        for(int k = 0; k < 4; k++) {
            if(!ha_json_str(json, keys[k], buf, sizeof(buf))) return false; // needs all four
            q.o[k] = buf;
        }

        // "Answer: B" -> 1. Anything else is not a usable question.
        if(!ha_json_str(json, "answer", buf, sizeof(buf)) || !buf[0]) return false;
        char c = buf[0];
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c < 'A' || c > 'D') return false;
        q.correct = (uint8_t)(c - 'A');

        tp.qs[tp.qcount] = q;
        tp.qcount++;
        return true;
    }

    // Map a wyr pack file's {a,b} keys into a WyrPrompt in the current pack.
    bool wyrLoadItem(const char* json) {
        if(_wyr.packCount == 0) return false;
        WyrPack& p = _wyr.packs[_wyr.packCount - 1];
        if(p.count >= PACK_MAX_ITEMS) return false;
        char buf[128];
        if(!ha_json_str(json, "a", buf, sizeof(buf))) return false;
        String a = buf;
        if(!ha_json_str(json, "b", buf, sizeof(buf))) return false;
        p.items[p.count].a = a;
        p.items[p.count].b = buf;
        p.count++;
        return true;
    }

    // Map a spectrum pack file's {left,right} keys into the current pack, reusing
    // WyrPrompt (a = left label, b = right label).
    bool spectrumLoadItem(const char* json) {
        if(_spec.packCount == 0) return false;
        WyrPack& p = _spec.packs[_spec.packCount - 1];
        if(p.count >= PACK_MAX_ITEMS) return false;
        char buf[128];
        if(!ha_json_str(json, "left", buf, sizeof(buf)) || !buf[0]) return false;
        String left = buf;
        if(!ha_json_str(json, "right", buf, sizeof(buf)) || !buf[0]) return false;
        p.items[p.count].a = left;
        p.items[p.count].b = buf;
        p.count++;
        return true;
    }

    // Map a scramble pack file's {word} key into the current pack.
    bool scrambleLoadItem(const char* json) {
        if(_scr.packCount == 0) return false;
        WordPack& p = _scr.packs[_scr.packCount - 1];
        if(p.count >= PACK_MAX_ITEMS) return false;
        char buf[24];
        if(!ha_json_str(json, "word", buf, sizeof(buf)) || !buf[0]) return false;
        p.words[p.count++] = buf;
        return true;
    }

    // Map a Kiss Marry Kill pack file's {name} key into the current pack.
    bool kmkLoadItem(const char* json) {
        if(_kmk.packCount == 0) return false;
        WordPack& p = _kmk.packs[_kmk.packCount - 1];
        if(p.count >= PACK_MAX_ITEMS) return false;
        char buf[40];
        if(!ha_json_str(json, "name", buf, sizeof(buf)) || !buf[0]) return false;
        p.words[p.count++] = buf;
        return true;
    }

    // Map a draw pack file's {word} key into the current pack.
    bool drawLoadItem(const char* json) {
        if(_d.packCount == 0) return false;
        WordPack& p = _d.packs[_d.packCount - 1];
        if(p.count >= PACK_MAX_ITEMS) return false;
        char buf[24];
        if(!ha_json_str(json, "word", buf, sizeof(buf)) || !buf[0]) return false;
        p.words[p.count++] = buf;
        return true;
    }

    void roundEnd() {
        _criticalPausePid = 0;
        clearActiveRound();
        pushAll();
    }

    // Time-based updates (trivia phases, drawing timers, pong physics). From loop().
    void tick(uint32_t rawNow) {
        advanceClock(rawNow);
        if(_clockPaused) return;
        bool rosterChanged = expireDetached(rawNow);
        if(_criticalPausePid) {
            if(rosterChanged) pushAll();
            return;
        }
        uint32_t now = clockNow();
        if(_active == HA_GAME_TRIVIA)
            triviaTick(now);
        else if(_active == HA_GAME_DRAW)
            drawTick(now);
        else if(_active == HA_GAME_PONG && (now - _lastPong) >= PONG_TICK_MS) {
            _lastPong = now;
            pongTick();
        } else if(_active == HA_GAME_WYR)
            wyrTick(now);
        else if(_active == HA_GAME_SCRAMBLE)
            scrambleTick(now);
        else if(_active == HA_GAME_REACT)
            reactTick(now);
        else if(_active == HA_GAME_GUESSCOLOR)
            gcTick(now);
        else if(_active == HA_GAME_SPECTRUM)
            spectrumTick(now);
        else if(_active == HA_GAME_KMK)
            kmkTick(now);
        else if(_active == HA_GAME_CHESS)
            chessTick(now);
        if(rosterChanged) pushAll();
    }

    // ---- player input (parsed WS JSON) ----
    void onInput(uint32_t wsId, const char* json, uint32_t rawNow) {
        advanceClock(rawNow);
        if(!ha_json_flat_object_valid(json)) return;
        char type[20];
        if(!ha_json_str(json, "t", type, sizeof(type))) return;
        if(strcmp(type, "hello") == 0) {
            int proto = 0;
            if(!ha_json_int(json, "proto", &proto) || proto != 2) {
                haWsSendWs(wsId, "{\"t\":\"reject\",\"code\":\"bad_protocol\",\"proto\":2}");
                return;
            }
            char nick[HA_NICK_LEN], avatar[8], resume[HA_RESUME_TOKEN_LEN + 1], code[7];
            ha_json_str(json, "nick", nick, sizeof(nick));
            if(!ha_json_str(json, "avatar", avatar, sizeof(avatar))) avatar[0] = '\0';
            if(!parseResumeToken(json, resume)) resume[0] = '\0';
            parseJoinCode(json, code);
            onHello(wsId, nick, avatar, resume, code, rawNow);
            return;
        }
        if(strcmp(type, "ping") == 0) {
            haWsSendWs(wsId, "{\"t\":\"pong\"}");
            return;
        }
        // Planned transport pauses must still admit returning browsers during the
        // host's reconnect window. Only game-changing input remains frozen.
        if(_clockPaused) return;
        if(expireDetached(rawNow)) pushAll();
        uint8_t pid = pidByWs(wsId);
        if(!pid) return;
        int v;
        if(strcmp(type, "react") == 0) {
            char emoji[8];
            if(ha_json_str(json, "emoji", emoji, sizeof(emoji))) onReact(pid, emoji);
            return;
        }
        if(strcmp(type, "say") == 0) {
            char t[120];
            if(ha_json_str(json, "text", t, sizeof(t))) onSay(pid, t);
            return;
        }
        if(_criticalPausePid) return;
        if(strcmp(type, "answer") == 0 && ha_json_int(json, "c", &v)) {
            triviaAnswer(pid, v);
            wyrAnswer(pid, v);
        } else if(strcmp(type, "ready") == 0) {
            const char* rp = ha_json_find(json, "ready");
            bool r = rp && strncmp(rp, "true", 4) == 0;
            triviaReady(pid, r);
            wyrReady(pid, r);
            scrambleReady(pid, r);
            reactReady(pid, r);
            gcReady(pid, r);
            spectrumReady(pid, r);
            kmkReady(pid, r);
        } else if(strcmp(type, "vote") == 0 && ha_json_int(json, "topic", &v)) {
            triviaVote(pid, v);
        } else if(strcmp(type, "vote") == 0 && ha_json_int(json, "pack", &v)) {
            wyrVote(pid, v);
            scrambleVote(pid, v);
            spectrumVote(pid, v);
            kmkVote(pid, v);
        } else if(strcmp(type, "tap") == 0) {
            reactTap(pid);
        } else if(strcmp(type, "clue") == 0) {
            char c[SPECTRUM_CLUE_LEN];
            if(ha_json_str(json, "text", c, sizeof(c))) spectrumClue(pid, c);
        } else if(strcmp(type, "slide") == 0 && ha_json_int(json, "n", &v)) {
            spectrumGuess(pid, v);
        } else if(strcmp(type, "assign") == 0) {
            int k, m, x;
            if(ha_json_int(json, "kiss", &k) && ha_json_int(json, "marry", &m) &&
               ha_json_int(json, "kill", &x))
                kmkAssign(pid, k, m, x);
        } else if(strcmp(type, "again") == 0) {
            triviaAgain(pid);
            drawAgain(pid);
            wyrAgain(pid);
            scrambleAgain(pid);
            reactAgain(pid);
            gcAgain(pid);
            spectrumAgain(pid);
            kmkAgain(pid);
        } else if(strcmp(type, "challenge") == 0 && ha_json_int(json, "to", &v) &&
                  v >= 1 && v <= HA_MAX_PLAYERS) {
            matchChallenge(pid, (uint8_t)v);
        } else if(strcmp(type, "accept") == 0 && ha_json_int(json, "id", &v) &&
                  v >= 1 && v <= 65535) {
            matchAccept(pid, v);
        } else if(strcmp(type, "cancel") == 0) {
            duelCancel(pid);
        } else if(strcmp(type, "move") == 0 && ha_json_int(json, "n", &v)) {
            duelMove(pid, v);
        } else if(strcmp(type, "move") == 0 && _active == HA_GAME_CHESS) {
            // Chess names its squares, so its "move" carries from/to instead of the
            // duels' single "n" and falls through to here.
            int from, to, promo;
            if(!ha_json_int(json, "from", &from)) from = -1;
            if(!ha_json_int(json, "to", &to)) to = -1;
            if(!ha_json_int(json, "promo", &promo)) promo = 0;
            chessMove(pid, from, to, promo);
        } else if(strcmp(type, "rematch") == 0) {
            if(isDuel(_active))
                duelRematch(pid);
            else if(_active == HA_GAME_BATTLESHIP)
                battleRematch(pid);
            else if(_active == HA_GAME_CHESS)
                chessRematch(pid);
        } else if(strcmp(type, "paddle") == 0 && ha_json_int(json, "dir", &v)) {
            pongPaddle(pid, v);
        } else if(strcmp(type, "place") == 0) {
            battlePlace(pid, json);
        } else if(strcmp(type, "fire") == 0 && ha_json_int(json, "n", &v)) {
            battleFire(pid, v);
        } else if(strcmp(type, "resign") == 0 && _active == HA_GAME_CHESS) {
            chessResign(pid);
        } else if(strcmp(type, "draw") == 0 && _active == HA_GAME_CHESS) {
            chessDraw(pid);
        } else if(strcmp(type, "claim") == 0 && _active == HA_GAME_CHESS) {
            chessClaim(pid);
        } else if(strcmp(type, "guess") == 0) {
            // A text guess (draw/scramble) or an r/g/b color guess (guess the color).
            char g[64];
            int r, gg, b;
            if(ha_json_str(json, "text", g, sizeof(g))) {
                drawGuess(pid, g);
                scrambleGuess(pid, g);
            } else if(
                ha_json_int(json, "r", &r) && ha_json_int(json, "g", &gg) &&
                ha_json_int(json, "b", &b)) {
                gcGuess(pid, r, gg, b);
            }
        } else if(strcmp(type, "stroke") == 0) {
            drawStroke(pid, json);
        } else if(strcmp(type, "clear") == 0) {
            drawClearInk(pid);
        } else if(strcmp(type, "leaveGame") == 0) {
            duelRemoveChallengesInvolving(pid);
            anyOnLeave(pid);
            pushAll();
        }
    }

private:
    Player _p[HA_MAX_PLAYERS + 1] = {};
    uint8_t _active = HA_GAME_NONE;
    char _session[HA_IDENTITY_LEN + 1] = {};
    char _lang[8] = {0}; // UI language code for the phone client, "" = English
    char _pendingLang[8] = {0};
    bool _pendingLangValid = false;
    uint32_t _clockNow = 0; // logical game time; frozen while transport is paused
    uint32_t _lastRawNow = 0;
    bool _clockStarted = false;
    bool _clockPaused = false;
    bool _pauseActive = false;
    char _pauseReason[16] = {0};
    char _pauseSsid[33] = {0};
    uint32_t _pauseReconnectMs = 0;
    uint8_t _criticalPausePid = 0; // Draw/Spectrum/KMK role whose grace freezes the round
    uint16_t _nextChallengeId = 1;
    Trivia _t = {};
    TriviaTopic* _topics = nullptr; // allocated only when trivia content is streamed
    uint8_t _topicCount = 0;
    uint8_t _packGame = 0; // HA_GAME_* of the pack currently being streamed, 0 = none
    DuelChallenge _c[DUEL_MAX_CHALLENGES] = {};
    DrawState _d = {};
    uint32_t _lastPong = 0;
    WyrState _wyr = {};
    ScrambleState _scr = {};
    ReactState _react = {};
    GuessColorState _gc = {};
    SpectrumState _spec = {};
    KmkState _kmk = {};
    MatchPools _matches = {};
    Engine* _contentStage = nullptr;
    // Kept separately from the pointer so an allocation failure still leaves a
    // real, abortable transaction. CONFIG must never leak its locale into the
    // live bank merely because the staging allocation failed.
    bool _contentTransactionOpen = false;
    uint16_t _contentStagePacks = 0;
    uint16_t _contentStageItems = 0;

    String serverPauseJson() const {
        return String("{\"t\":\"server_pause\",\"reason\":\"") +
               ha_json_escape(_pauseReason[0] ? _pauseReason : "ap_off") + "\",\"ssid\":\"" +
               ha_json_escape(_pauseSsid) + "\",\"reconnect_ms\":" +
               String((unsigned long)_pauseReconnectMs) + "}";
    }

    // Content is bulk streamed only for games with installed packs. Keeping every
    // possible String-heavy pack table embedded in the global Engine consumed tens
    // of KiB of DRAM before a session even started. Allocate each table on its first
    // pack and release it on CONTENT_CLEAR; packCount remains the validity guard.
    template <typename T>
    static bool ensureContentStorage(T*& storage) {
        if(!storage) storage = new(std::nothrow) T[TRIVIA_MAX_TOPICS]{};
        return storage != nullptr;
    }

    void discardContentStage() {
        if(_contentStage) {
            _contentStage->clearContentStorage();
            delete _contentStage;
        }
        _contentStage = nullptr;
        _contentTransactionOpen = false;
        _contentStagePacks = 0;
        _contentStageItems = 0;
        _pendingLangValid = false;
    }

    void clearActiveRound() {
        if(_active == HA_GAME_TRIVIA)
            triviaClear();
        else if(isDuel(_active))
            duelClear();
        else if(_active == HA_GAME_DRAW)
            drawClear();
        else if(_active == HA_GAME_PONG)
            pongClear();
        else if(_active == HA_GAME_WYR)
            wyrClear();
        else if(_active == HA_GAME_SCRAMBLE)
            scrambleClear();
        else if(_active == HA_GAME_REACT)
            reactClear();
        else if(_active == HA_GAME_GUESSCOLOR)
            gcClear();
        else if(_active == HA_GAME_BATTLESHIP)
            battleClear();
        else if(_active == HA_GAME_SPECTRUM)
            spectrumClear();
        else if(_active == HA_GAME_KMK)
            kmkClear();
        else if(_active == HA_GAME_CHESS)
            chessClear();
    }

    void advanceClock(uint32_t rawNow) {
        if(!_clockStarted) {
            _lastRawNow = rawNow;
            _clockStarted = true;
            return;
        }
        uint32_t elapsed = rawNow - _lastRawNow;
        _lastRawNow = rawNow;
        if(!_clockPaused && !_criticalPausePid) _clockNow += elapsed;
    }
    uint32_t clockNow() const { return _clockNow; }

    bool matchBothOnline(uint8_t a, uint8_t b) const {
        return playerOnline(a) && playerOnline(b);
    }

    bool isCriticalRole(uint8_t pid) const {
        if(_active == HA_GAME_DRAW)
            return _d.phase == 1 && _d.drawer == pid;
        if(_active == HA_GAME_SPECTRUM)
            return _spec.pt.phase == 2 && _spec.stage == 0 && _spec.psychic == pid;
        if(_active == HA_GAME_KMK)
            return _kmk.pt.phase == 2 && _kmk.stage == 0 && _kmk.chooser == pid;
        return false;
    }

    void pauseAffectedGame(uint8_t pid, uint32_t rawNow) {
        if(!_criticalPausePid && isCriticalRole(pid)) {
            _criticalPausePid = pid;
            _lastRawNow = rawNow;
        }
        if(_active == HA_GAME_PONG) {
            PongMatch* m = pongMatchOf(pid);
            if(m && m->phase == 1) m->d1 = m->d2 = 0;
        } else if(_active == HA_GAME_CHESS) {
            ChessMatch* m = chessMatchOf(pid);
            if(m && m->phase == 1 && !m->clockPaused) {
                uint8_t side = m->core.stm;
                uint32_t elapsed = clockNow() - m->lastStamp;
                if(elapsed >= m->clockMs[side]) {
                    chessFlagFall(m);
                } else {
                    m->clockMs[side] -= elapsed;
                    m->lastStamp = clockNow();
                    m->clockPaused = true;
                }
            }
        }
    }

    void resumeAffectedGame(uint8_t pid, uint32_t rawNow) {
        if(_criticalPausePid == pid) {
            _criticalPausePid = 0;
            _lastRawNow = rawNow;
            _clockStarted = true;
        }
        if(_active == HA_GAME_CHESS) {
            ChessMatch* m = chessMatchOf(pid);
            if(m && m->phase == 1 && m->clockPaused && matchBothOnline(m->a, m->b)) {
                m->lastStamp = clockNow();
                m->clockPaused = false;
            }
        }
    }

    void appendTimer(String& json, uint32_t deadline, uint32_t durationMs) const {
        json += ",\"remaining_ms\":";
        json += (unsigned long)haTimeRemaining(_clockNow, deadline);
        json += ",\"duration_ms\":";
        json += (unsigned long)durationMs;
    }

    bool playerOnline(uint8_t pid) const {
        return pid >= 1 && pid <= HA_MAX_PLAYERS && _p[pid].used && _p[pid].wsId != 0;
    }

    void awardScore(uint8_t pid, int delta, const char* reason) {
        if(pid < 1 || pid > HA_MAX_PLAYERS || !_p[pid].used || delta == 0) return;
        int32_t before = _p[pid].score;
        int64_t next = (int64_t)before + delta;
        if(next > 2147483647LL) next = 2147483647LL;
        if(next < -2147483648LL) next = -2147483648LL;
        _p[pid].score = (int32_t)next;
        int applied = (int)(_p[pid].score - before);
        if(applied) haUartScore(pid, applied, reason);
    }

    void hostEvent(uint8_t kind, uint8_t actor = 0, uint8_t target = 0,
                   int16_t value = 0, const char* text = "") {
        // The UART event contract is deliberately bounded.  Trim at a complete
        // UTF-8 code point so every consumer receives valid text even when a chat
        // message lands exactly on the wire limit.
        const char* src = text ? text : "";
        char bounded[HA_HOST_EVENT_TEXT_MAX + 1];
        size_t in = 0, out = 0;
        while(src[in] && out < HA_HOST_EVENT_TEXT_MAX) {
            unsigned char lead = (unsigned char)src[in];
            size_t width = lead < 0x80 ? 1 :
                           (lead >= 0xC2 && lead <= 0xDF) ? 2 :
                           (lead >= 0xE0 && lead <= 0xEF) ? 3 :
                           (lead >= 0xF0 && lead <= 0xF4) ? 4 : 1;
            bool complete = true;
            for(size_t j = 1; j < width; j++) {
                unsigned char c = (unsigned char)src[in + j];
                if(!c || (c & 0xC0) != 0x80) { complete = false; break; }
            }
            if(!complete) width = 1;
            if(out + width > HA_HOST_EVENT_TEXT_MAX) break;
            memcpy(bounded + out, src + in, width);
            in += width;
            out += width;
        }
        bounded[out] = '\0';
        haUartHostEvent(kind, _active, actor, target, value, bounded);
    }

    static bool validResumeToken(const char* token) {
        if(!token || strlen(token) != HA_RESUME_TOKEN_LEN) return false;
        for(int i = 0; i < HA_RESUME_TOKEN_LEN; i++) {
            char c = token[i];
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    static bool parseResumeToken(const char* json, char out[HA_RESUME_TOKEN_LEN + 1]) {
        return ha_json_str(json, "resume", out, HA_RESUME_TOKEN_LEN + 1) &&
               validResumeToken(out);
    }

    // A generic bounded JSON-string helper silently truncates a seven-or-more
    // digit value into a seemingly valid six-digit code. Parse this fixed-width
    // admission credential exactly so malformed values can never authenticate.
    static void parseJoinCode(const char* json, char out[7]) {
        out[0] = '\0';
        if(!ha_json_find(json, "code")) return;
        if(!ha_json_str(json, "code", out, 7)) {
            out[0] = '!'; out[1] = '\0'; return;
        }
        for(int i = 0; i < 6; i++) {
            if(out[i] < '0' || out[i] > '9') {
                out[0] = '!';
                out[1] = '\0';
                return;
            }
        }
        if(out[6] != '\0') { out[0] = '!'; out[1] = '\0'; }
    }

    static void makeSessionId(char out[HA_IDENTITY_LEN + 1]) {
        static const char HEX_DIGITS[] = "0123456789abcdef";
        for(int word = 0; word < 4; word++) {
            uint32_t r = esp_random();
            for(int nib = 0; nib < 8; nib++)
                out[word * 8 + nib] = HEX_DIGITS[(r >> ((7 - nib) * 4)) & 0x0F];
        }
        out[HA_IDENTITY_LEN] = '\0';
    }

    uint8_t pidByIdentity(const char* identity) const {
        if(!identity || strlen(identity) != HA_IDENTITY_LEN) return 0;
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
            if(_p[pid].used && strcmp(_p[pid].identity, identity) == 0) return pid;
        return 0;
    }

    void clearPidState(uint8_t pid) {
        if(pid > HA_MAX_PLAYERS) return;
        _t.ready[pid] = false; _t.vote[pid] = -1; _t.answer[pid] = -1;
        _t.answerMs[pid] = 0; _t.gained[pid] = 0; _d.vote[pid] = -1;
        _wyr.pt.ready[pid] = false; _wyr.vote[pid] = -1; _wyr.choice[pid] = -1;
        _scr.pt.ready[pid] = false; _scr.solved[pid] = false; _scr.vote[pid] = -1;
        _react.pt.ready[pid] = false; _react.tapped[pid] = false; _react.dq[pid] = false;
        _gc.pt.ready[pid] = false; _gc.guessed[pid] = false;
        _gc.gr[pid] = _gc.gg[pid] = _gc.gb[pid] = 0;
        _gc.submitMs[pid] = 0; _gc.gained[pid] = 0;
        _spec.pt.ready[pid] = false; _spec.vote[pid] = -1;
        _spec.guess[pid] = -1; _spec.gained[pid] = 0;
        _kmk.pt.ready[pid] = false; _kmk.vote[pid] = -1;
        for(int k = 0; k < 3; k++) _kmk.gLabel[pid][k] = -1;
        _kmk.guessed[pid] = false; _kmk.gained[pid] = 0;
    }

    // Global role fields also carry pids. Once a grace-period seat is truly
    // released, make sure a brand-new player reusing that number cannot inherit
    // an old secret role or winner badge.
    void releasePidRoles(uint8_t pid) {
        if(_d.drawer == pid) {
            if(_d.phase == 1) {
                _d.phase = 2;
                _d.winner = 0;
                _d.revealUntil = clockNow() + DRAW_REVEAL_MS;
            }
            _d.drawer = 0;
        }
        if(_d.winner == pid) _d.winner = 0;
        if(_react.winner == pid) _react.winner = 0;
        if(_gc.winner == pid) _gc.winner = 0;
        if(_spec.psychic == pid) {
            _spec.psychic = 0;
            if(_spec.pt.phase == 2 && _spec.stage == 0) {
                _spec.stage = 1;
                _spec.pt.deadline = clockNow() + (uint32_t)SPECTRUM_GUESS_SECS * 1000;
            }
        }
        if(_kmk.chooser == pid) {
            _kmk.chooser = 0;
            if(_kmk.pt.phase == 2 && _kmk.stage == 0) {
                _kmk.cLabel[0] = 0;
                _kmk.cLabel[1] = 1;
                _kmk.cLabel[2] = 2;
                _kmk.stage = 1;
                _kmk.pt.deadline = clockNow() + (uint32_t)KMK_GUESS_SECS * 1000;
            }
        }
    }

    void finalizeLeave(uint8_t pid) {
        if(pid < 1 || pid > HA_MAX_PLAYERS || !_p[pid].used) return;
        resumeAffectedGame(pid, _lastRawNow);
        anyOnLeave(pid);
        duelRemoveChallengesInvolving(pid);
        releasePidRoles(pid);
        clearPidState(pid);
        _p[pid] = Player{};
        haUartLeave(pid);
    }

    bool expireDetached(uint32_t rawNow) {
        bool changed = false;
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(!_p[pid].used || !_p[pid].detached) continue;
            if((uint32_t)(rawNow - _p[pid].detachedAt) < HA_RESUME_GRACE_MS) continue;
            finalizeLeave(pid);
            changed = true;
        }
        if(changed) { triviaOnRosterChange(); partyRosterChanged(); }
        return changed;
    }

    void drawResetRotation() {
        _d.lastDrawer = 0;
        _d.nextPack = 0;
        for(int i = 0; i < TRIVIA_MAX_TOPICS; i++) {
            _d.wordSeq[i] = 0;
            _d.lastWord[i] = 0xFF;
        }
    }

    uint8_t freePid() {
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(!_p[i].used) return i;
        return 0;
    }
    int connectedCount() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i)) n++;
        return n;
    }

    // ---------- broadcast ----------
    void pushAll() {
        String lob = lobbyJson();
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(!_p[pid].used || !_p[pid].wsId) continue;
            haWsSendWs(_p[pid].wsId, lob);
            if(_active == HA_GAME_TRIVIA)
                haWsSendWs(_p[pid].wsId, triviaJson(pid));
            else if(isDuel(_active))
                haWsSendWs(_p[pid].wsId, duelJson(pid));
            else if(_active == HA_GAME_DRAW)
                haWsSendWs(_p[pid].wsId, drawJson(pid));
            else if(_active == HA_GAME_PONG)
                haWsSendWs(_p[pid].wsId, pongJson(pid));
            else if(_active == HA_GAME_WYR)
                haWsSendWs(_p[pid].wsId, wyrJson(pid));
            else if(_active == HA_GAME_SCRAMBLE)
                haWsSendWs(_p[pid].wsId, scrambleJson(pid));
            else if(_active == HA_GAME_REACT)
                haWsSendWs(_p[pid].wsId, reactJson(pid));
            else if(_active == HA_GAME_GUESSCOLOR)
                haWsSendWs(_p[pid].wsId, gcJson(pid));
            else if(_active == HA_GAME_BATTLESHIP)
                haWsSendWs(_p[pid].wsId, battleJson(pid));
            else if(_active == HA_GAME_SPECTRUM)
                haWsSendWs(_p[pid].wsId, spectrumJson(pid));
            else if(_active == HA_GAME_KMK)
                haWsSendWs(_p[pid].wsId, kmkJson(pid));
            else if(_active == HA_GAME_CHESS)
                haWsSendWs(_p[pid].wsId, chessJson(pid));
        }
    }

    String playersJson() {
        String s = "[";
        bool first = true;
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(!_p[pid].used) continue;
            if(!first) s += ",";
            s += "{\"pid\":";
            s += pid;
            s += ",\"nick\":\"";
            s += ha_json_escape(_p[pid].nick);
            s += "\",\"avatar\":\"";
            s += ha_json_escape(_p[pid].avatar);
            s += "\",\"score\":";
            s += _p[pid].score;
            s += ",\"online\":";
            s += playerOnline(pid) ? "true" : "false";
            // In a 1v1 match (playing OR still on the over screen): don't let others
            // challenge them until they return to the lobby.
            s += ",\"busy\":";
            s += (inAnyMatch(pid) || !playerOnline(pid)) ? "true" : "false";
            s += "}";
            first = false;
        }
        s += "]";
        return s;
    }

    static const char* gameName(uint8_t g) {
        switch(g) {
        case HA_GAME_TRIVIA:
            return "trivia";
        case HA_GAME_CONNECT4:
            return "connect4";
        case HA_GAME_TICTACTOE:
            return "tictactoe";
        case HA_GAME_DOTS:
            return "dots";
        case HA_GAME_DRAW:
            return "draw";
        case HA_GAME_PONG:
            return "pong";
        case HA_GAME_REACT:
            return "react";
        case HA_GAME_WYR:
            return "wyr";
        case HA_GAME_SCRAMBLE:
            return "scramble";
        case HA_GAME_REVERSI:
            return "reversi";
        case HA_GAME_GUESSCOLOR:
            return "gc";
        case HA_GAME_BATTLESHIP:
            return "bs";
        case HA_GAME_SPECTRUM:
            return "spectrum";
        case HA_GAME_KMK:
            return "kmk";
        case HA_GAME_CHESS:
            return "chess";
        default:
            return "none";
        }
    }

    String lobbyJson() {
        return String("{\"t\":\"lobby\",\"game\":\"") + gameName(_active) +
               "\",\"players\":" + playersJson() + "}";
    }

    static bool isDuel(uint8_t g) {
        return g == HA_GAME_CONNECT4 || g == HA_GAME_TICTACTOE || g == HA_GAME_DOTS ||
               g == HA_GAME_REVERSI;
    }

    // ---------- trivia (phone-driven, self-organizing) ----------
    // Pull the four strings of "o":[...] in order into opts[4].
    static void parseOptions(const char* json, String opts[4]) {
        const char* q = ha_json_find(json, "o");
        if(!q || *q != '[') return;
        q++;
        for(int k = 0; k < 4 && *q; k++) {
            while(*q == ' ' || *q == ',') q++;
            if(*q != '"') break;
            q++;
            String s;
            while(*q && *q != '"') {
                if(*q == '\\' && q[1]) {
                    q++;
                    s += *q;
                } else {
                    s += *q;
                }
                q++;
            }
            opts[k] = s;
            if(*q == '"') q++;
        }
    }

    void triviaClear() {
        _t.phase = 0; // lobby
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _t.ready[i] = false;
            _t.vote[i] = -1;
            _t.answer[i] = -1;
            _t.answerMs[i] = 0;
            _t.gained[i] = 0;
        }
        for(int k = 0; k < 4; k++) _t.counts[k] = 0;
        _t.qi = 0;
        _t.topic = 0;
        _t.lastSec = -1;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used) _p[i].score = 0;
    }

    bool triviaAllReady() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(!_t.ready[i]) return false;
        }
        return n >= 1;
    }

    bool triviaAllAnswered() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(_t.answer[i] < 0) return false;
        }
        return n >= 1;
    }

    void triviaCheckStart() {
        if(_active != HA_GAME_TRIVIA) return;
        if(_t.phase == 0 && _topicCount > 0 && triviaAllReady()) {
            _t.phase = 1; // all ready -> countdown
            // Lock in the winning topic now (votes are frozen during the
            // countdown) so the countdown shows the right name and the questions
            // come from the same topic (recomputing could break a random tie).
            _t.topic = (uint8_t)triviaWinningTopic();
            _t.countdownEnd = clockNow() + (uint32_t)TRIVIA_COUNTDOWN * 1000;
            _t.lastSec = -1;
        } else if(_t.phase == 1 && !triviaAllReady()) {
            _t.phase = 0; // someone unreadied / a new player joined -> cancel
        }
    }

    void triviaOnRosterChange() {
        if(_active != HA_GAME_TRIVIA) return;
        triviaCheckStart();
        if(_t.phase == 2 && triviaAllAnswered()) triviaDoReveal();
    }

    void triviaReady(uint8_t pid, bool r) {
        if(_active != HA_GAME_TRIVIA || (_t.phase != 0 && _t.phase != 1)) return;
        _t.ready[pid] = r;
        triviaCheckStart();
        pushAll();
    }

    void triviaVote(uint8_t pid, int topic) {
        if(_active != HA_GAME_TRIVIA || _t.phase != 0) return;
        if(topic < 0 || topic >= _topicCount) return;
        _t.vote[pid] = (int8_t)topic;
        pushAll();
    }

    int triviaWinningTopic() {
        int votes[TRIVIA_MAX_TOPICS] = {0};
        int total = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i) && _t.vote[i] >= 0 && _t.vote[i] < _topicCount) {
                votes[_t.vote[i]]++;
                total++;
            }
        if(total == 0) return (int)random(_topicCount);
        int best = 0;
        for(int i = 1; i < _topicCount; i++)
            if(votes[i] > votes[best]) best = i;
        int tie[TRIVIA_MAX_TOPICS], tn = 0;
        for(int i = 0; i < _topicCount; i++)
            if(votes[i] == votes[best]) tie[tn++] = i;
        return tie[(int)random(tn)];
    }

    void triviaStartQuestion() {
        _t.phase = 2;
        _t.deadline = clockNow() + (uint32_t)TRIVIA_QDUR * 1000;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _t.answer[i] = -1;
            _t.answerMs[i] = 0;
            _t.gained[i] = 0;
        }
        for(int k = 0; k < 4; k++) _t.counts[k] = 0;
        pushAll();
    }

    void triviaBeginGame() {
        // _t.topic was locked in when the countdown started (triviaCheckStart).
        _t.qi = 0;
        triviaStartQuestion();
    }

    int triviaPoints(uint32_t answeredAt) {
        uint32_t start = _t.deadline - (uint32_t)TRIVIA_QDUR * 1000;
        uint32_t elapsed = answeredAt - start;
        uint32_t total = (uint32_t)TRIVIA_QDUR * 1000;
        if(elapsed > total) elapsed = total;
        int bonus = (int)(500UL * (total - elapsed) / (total ? total : 1));
        return 500 + bonus;
    }

    void triviaAnswer(uint8_t pid, int c) {
        if(_active != HA_GAME_TRIVIA || _t.phase != 2) return;
        if(c < 0 || c > 3 || _t.answer[pid] >= 0 || haTimeReached(clockNow(), _t.deadline))
            return;
        _t.answer[pid] = (int8_t)c;
        _t.answerMs[pid] = clockNow();
        _t.counts[c]++;
        if(triviaAllAnswered())
            triviaDoReveal();
        else
            pushAll();
    }

    void triviaDoReveal() {
        _t.phase = 3;
        uint8_t correct = _topics[_t.topic].qs[_t.qi].correct;
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(!_p[pid].used || _t.answer[pid] < 0) continue;
            if(_t.answer[pid] == correct) {
                int pts = triviaPoints(_t.answerMs[pid]);
                _t.gained[pid] = pts;
                awardScore(pid, pts, "trivia");
            }
        }
        _t.revealUntil = clockNow() + TRIVIA_REVEAL_MS;
        pushAll();
    }

    void triviaNext() {
        _t.qi++;
        if(_t.qi >= _topics[_t.topic].qcount) {
            _t.phase = 4; // final
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
        } else {
            triviaStartQuestion();
        }
    }

    void triviaAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_TRIVIA || _t.phase != 4) return;
        triviaClear(); // back to the lobby (scores reset)
        pushAll();
    }

    void triviaTick(uint32_t now) {
        if(_t.phase == 1) { // countdown
            if(haTimeReached(now, _t.countdownEnd)) {
                triviaBeginGame();
                return;
            }
            int secs = (int)((haTimeRemaining(now, _t.countdownEnd) + 999) / 1000);
            if(secs != _t.lastSec) {
                _t.lastSec = secs;
                pushAll(); // client shows the new second + plays a tick
            }
        } else if(_t.phase == 2) { // question
            if(haTimeReached(now, _t.deadline)) triviaDoReveal();
        } else if(_t.phase == 3) { // reveal
            if(haTimeReached(now, _t.revealUntil)) triviaNext();
        }
    }

    // Leaderboard: connected players sorted by score desc.
    String triviaBoard() {
        uint8_t order[HA_MAX_PLAYERS];
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used) order[n++] = i;
        for(int a = 0; a < n; a++)
            for(int b = a + 1; b < n; b++)
                if(_p[order[b]].score > _p[order[a]].score) {
                    uint8_t t = order[a];
                    order[a] = order[b];
                    order[b] = t;
                }
        String s = "[";
        for(int i = 0; i < n; i++) {
            if(i) s += ",";
            s += "{\"pid\":";
            s += order[i];
            s += ",\"nick\":\"";
            s += ha_json_escape(_p[order[i]].nick);
            s += "\",\"avatar\":\"";
            s += ha_json_escape(_p[order[i]].avatar);
            s += "\",\"score\":";
            s += _p[order[i]].score;
            s += "}";
        }
        s += "]";
        return s;
    }

    String triviaJson(uint8_t pid) {
        if(_t.phase == 0) { // lobby: ready + topic vote
            String s = "{\"t\":\"trivia\",\"phase\":\"lobby\",\"players\":[";
            bool first = true;
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
                if(!_p[i].used) continue;
                if(!first) s += ",";
                first = false;
                s += "{\"pid\":";
                s += i;
                s += ",\"nick\":\"";
                s += ha_json_escape(_p[i].nick);
                s += "\",\"avatar\":\"";
                s += ha_json_escape(_p[i].avatar);
                s += "\",\"ready\":";
                s += _t.ready[i] ? "true" : "false";
                s += ",\"online\":";
                s += playerOnline(i) ? "true" : "false";
                s += "}";
            }
            s += "],\"topics\":[";
            int votes[TRIVIA_MAX_TOPICS] = {0};
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _t.vote[i] >= 0 && _t.vote[i] < _topicCount)
                    votes[_t.vote[i]]++;
            for(int i = 0; i < _topicCount; i++) {
                if(i) s += ",";
                s += "{\"name\":\"";
                s += ha_json_escape(_topics[i].name.c_str());
                s += "\",\"votes\":";
                s += votes[i];
                s += "}";
            }
            s += "],\"myvote\":";
            s += _t.vote[pid];
            s += ",\"myready\":";
            s += _t.ready[pid] ? "true" : "false";
            s += "}";
            return s;
        }
        if(_t.phase == 1) { // countdown
            uint32_t now = clockNow();
            int secs = haTimeReached(now, _t.countdownEnd)
                           ? 1
                           : (int)((haTimeRemaining(now, _t.countdownEnd) + 999) / 1000);
            if(secs < 1) secs = 1;
            return String("{\"t\":\"trivia\",\"phase\":\"countdown\",\"secs\":") + secs +
                   ",\"topic\":\"" + ha_json_escape(_topics[_t.topic].name.c_str()) + "\"}";
        }
        if(_t.phase == 4) { // final
            return String("{\"t\":\"trivia\",\"phase\":\"final\",\"board\":") + triviaBoard() + "}";
        }
        // question / reveal
        TriviaTopic& tp = _topics[_t.topic];
        TriviaQ& q = tp.qs[_t.qi];
        const char* phase = (_t.phase == 3) ? "reveal" : "question";
        String s = String("{\"t\":\"trivia\",\"phase\":\"") + phase + "\",\"i\":" + _t.qi +
                   ",\"n\":" + tp.qcount + ",\"q\":\"" + ha_json_escape(q.q.c_str()) + "\",\"o\":[";
        for(int k = 0; k < 4; k++) {
            if(k) s += ",";
            s += "\"";
            s += ha_json_escape(q.o[k].c_str());
            s += "\"";
        }
        s += "],\"mine\":";
        s += _t.answer[pid];
        s += ",\"topic\":\"";
        s += ha_json_escape(tp.name.c_str());
        s += "\",\"board\":" + triviaBoard();
        if(_t.phase == 2) {
            int answered = 0;
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _t.answer[i] >= 0) answered++;
            appendTimer(s, _t.deadline, (uint32_t)TRIVIA_QDUR * 1000);
            s += ",\"answered\":";
            s += answered;
            s += ",\"total\":";
            s += connectedCount();
        } else { // reveal
            s += ",\"correct\":";
            s += q.correct;
            s += ",\"counts\":[";
            for(int k = 0; k < 4; k++) {
                if(k) s += ",";
                s += _t.counts[k];
            }
            s += "],\"gained\":";
            s += _t.gained[pid];
        }
        s += "}";
        return s;
    }

    // ---------- duels (connect4 / tic-tac-toe / dots) ----------
    void duelClear() {
        memset(&_matches, 0, sizeof(_matches));
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++) _c[i] = DuelChallenge{};
    }

    static const char* kindStr(uint8_t kind) {
        return kind == HA_GAME_TICTACTOE ? "ttt" :
               kind == HA_GAME_DOTS      ? "dots" :
               kind == HA_GAME_REVERSI   ? "reversi" :
                                           "c4";
    }

    // Grid params for c4/ttt.
    static void gridParams(uint8_t kind, int& cols, int& rows, int& need, bool& gravity) {
        if(kind == HA_GAME_TICTACTOE) {
            cols = 3;
            rows = 3;
            need = 3;
            gravity = false;
        } else { // connect4
            cols = 7;
            rows = 6;
            need = 4;
            gravity = true;
        }
    }

    DuelMatch* matchOf(uint8_t pid) {
        for(int i = 0; i < DUEL_MAX_MATCHES; i++) {
            if(!_matches.duel[i].used) continue;
            if(_matches.duel[i].a == pid && _matches.duel[i].aIn) return &_matches.duel[i];
            if(_matches.duel[i].b == pid && _matches.duel[i].bIn) return &_matches.duel[i];
        }
        return nullptr;
    }

    void duelRemoveChallengesInvolving(uint8_t pid) {
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++)
            if(_c[i].used && (_c[i].from == pid || _c[i].to == pid)) _c[i] = DuelChallenge{};
    }

    uint16_t allocChallengeId() {
        for(uint32_t tries = 0; tries < 0xFFFFUL; tries++) {
            uint16_t id = _nextChallengeId++;
            if(_nextChallengeId == 0) _nextChallengeId = 1;
            if(id == 0) continue;
            bool collision = false;
            for(int i = 0; i < DUEL_MAX_CHALLENGES; i++)
                if(_c[i].used && _c[i].id == id) { collision = true; break; }
            if(!collision) return id;
        }
        return 0;
    }

    // Challenge/accept are shared by all 1v1 games (duels + pong + battleship + chess).
    bool isMatchGame() {
        return isDuel(_active) || _active == HA_GAME_PONG ||
               _active == HA_GAME_BATTLESHIP || _active == HA_GAME_CHESS;
    }
    bool inAnyMatch(uint8_t pid) {
        if(isDuel(_active)) return matchOf(pid) != nullptr;
        if(_active == HA_GAME_PONG) return pongMatchOf(pid) != nullptr;
        if(_active == HA_GAME_BATTLESHIP) return battleMatchOf(pid) != nullptr;
        if(_active == HA_GAME_CHESS) return chessMatchOf(pid) != nullptr;
        return false;
    }

    void matchChallenge(uint8_t from, uint8_t to) {
        if(!isMatchGame()) return;
        if(to == from || to < 1 || to > HA_MAX_PLAYERS || !playerOnline(from) ||
           !playerOnline(to))
            return;
        if(inAnyMatch(from) || inAnyMatch(to)) return;
        // one outstanding challenge per challenger
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++)
            if(_c[i].used && _c[i].from == from) _c[i] = DuelChallenge{};
        bool allocated = false;
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++) {
            if(!_c[i].used) {
                uint16_t id = allocChallengeId();
                if(!id) break;
                _c[i] = DuelChallenge{true, id, _active, from, to};
                allocated = true;
                break;
            }
        }
        if(!allocated) {
            if(_p[from].wsId)
                haWsSendWs(_p[from].wsId,
                           "{\"t\":\"error\",\"code\":\"challenge_capacity\"}");
            return;
        }
        if(_p[to].wsId)
            haWsSendWs(
                _p[to].wsId,
                String("{\"t\":\"toast\",\"msg\":\"") + ha_json_escape(_p[from].nick) +
                    " challenges you\"}");
        pushAll();
    }

    // Set up a fresh match between a (mark 1) and b (mark 2); `first` moves first.
    void duelStart(DuelMatch* m, uint8_t a, uint8_t b, uint8_t first) {
        *m = DuelMatch{};
        m->used = true;
        m->kind = _active;
        m->a = a;
        m->b = b;
        m->aIn = m->bIn = true;
        m->turn = first;
        m->first = first;
        m->phase = 1;
        m->winner = 0;
        if(m->kind == HA_GAME_REVERSI) {
            // 8x8 with the four center starting discs (black=1 = mark a, white=2 = mark b).
            // Standard opening: d5,e4 black; d4,e5 white. Black (challenger) moves first.
            m->board[3 * 8 + 3] = 2; // d4 white
            m->board[3 * 8 + 4] = 1; // e4 black
            m->board[4 * 8 + 3] = 1; // d5 black
            m->board[4 * 8 + 4] = 2; // e5 white
        }
    }

    void matchAccept(uint8_t pid, int challengeId) {
        if(!isMatchGame()) return;
        uint8_t from = 0;
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++)
            if(_c[i].used && _c[i].id == challengeId && _c[i].to == pid &&
               _c[i].game == _active) {
                from = _c[i].from;
                break;
            }
        if(!from || !playerOnline(pid) || !playerOnline(from)) return;
        if(inAnyMatch(pid) || inAnyMatch(from)) {
            duelRemoveChallengesInvolving(pid);
            duelRemoveChallengesInvolving(from);
            pushAll();
            return;
        }
        bool allocated = false;
        if(_active == HA_GAME_PONG) {
            for(int i = 0; i < PONG_MAX; i++)
                if(!_matches.pong[i].used) {
                    pongStart(&_matches.pong[i], from, pid);
                    allocated = true;
                    break;
                }
        } else if(_active == HA_GAME_BATTLESHIP) {
            for(int i = 0; i < BATTLE_MAX; i++)
                if(!_matches.battle[i].used) {
                    battleStart(&_matches.battle[i], from, pid, from); // challenger fires first
                    allocated = true;
                    break;
                }
        } else if(_active == HA_GAME_CHESS) {
            for(int i = 0; i < CHESS_MAX; i++)
                if(!_matches.chess[i].used) {
                    chessStart(&_matches.chess[i], from, pid, from); // challenger plays white
                    allocated = true;
                    break;
                }
        } else {
            for(int i = 0; i < DUEL_MAX_MATCHES; i++)
                if(!_matches.duel[i].used) {
                    duelStart(&_matches.duel[i], from, pid, from); // challenger moves first
                    allocated = true;
                    break;
                }
        }
        if(!allocated) {
            const String err = "{\"t\":\"error\",\"code\":\"match_capacity\"}";
            if(_p[from].wsId) haWsSendWs(_p[from].wsId, err);
            if(_p[pid].wsId) haWsSendWs(_p[pid].wsId, err);
            pushAll();
            return;
        }
        // Acceptance consumes the invitation only once an authoritative match
        // slot exists. A full table leaves the challenge available for retry.
        duelRemoveChallengesInvolving(pid);
        duelRemoveChallengesInvolving(from);
        hostEvent(HA_HOST_EVT_MATCH_STARTED, from, pid);
        pushAll();
    }

    void anyOnLeave(uint8_t pid) {
        if(isDuel(_active))
            duelOnLeave(pid);
        else if(_active == HA_GAME_PONG)
            pongOnLeave(pid);
        else if(_active == HA_GAME_BATTLESHIP)
            battleOnLeave(pid);
        else if(_active == HA_GAME_CHESS)
            chessOnLeave(pid);
    }

    void duelCancel(uint8_t pid) {
        duelRemoveChallengesInvolving(pid);
        pushAll();
    }

    // Rematch: in an over match, restart the same pairing with the first move
    // alternated. Only if the opponent is still attached.
    void duelRematch(uint8_t pid) {
        DuelMatch* m = matchOf(pid);
        if(!m || m->phase != 2) return;
        if(!matchBothOnline(m->a, m->b)) return;
        if(!m->aIn || !m->bIn) {
            // Opponent has left: there is no one to rematch. Send this player back to
            // the lobby with a note, rather than silently doing nothing.
            if(_p[pid].wsId)
                haWsSendWs(_p[pid].wsId, String("{\"t\":\"toast\",\"msg\":\"Opponent left\"}"));
            duelOnLeave(pid);
            pushAll();
            return;
        }
        uint8_t next = (m->first == m->a) ? m->b : m->a;
        duelStart(m, m->a, m->b, next);
        pushAll();
    }

    void duelMove(uint8_t pid, int n) {
        DuelMatch* m = matchOf(pid);
        if(!m || m->phase != 1 || m->turn != pid || !matchBothOnline(m->a, m->b)) return;
        uint8_t mark = (pid == m->a) ? 1 : 2;
        if(m->kind == HA_GAME_DOTS)
            dotsMove(m, pid, n, mark);
        else if(m->kind == HA_GAME_REVERSI)
            reversiMove(m, pid, n, mark);
        else
            gridMove(m, pid, n, mark);
        pushAll();
    }

    // ---- reversi / othello (8x8, capture by flanking) ----
    // 8 ray directions (row,col deltas), shared by the flip helpers.
    static const int* revDR() { static const int d[8] = {-1, -1, -1, 0, 0, 1, 1, 1}; return d; }
    static const int* revDC() { static const int d[8] = {-1, 0, 1, -1, 1, -1, 0, 1}; return d; }

    // How many opponent discs a move at (r,c) by `mark` would flip (0 = illegal).
    static int reversiFlips(const uint8_t* b, int r, int c, uint8_t mark) {
        if(b[r * 8 + c] != 0) return 0;
        const int *DR = revDR(), *DC = revDC();
        uint8_t opp = (mark == 1) ? 2 : 1;
        int total = 0;
        for(int d = 0; d < 8; d++) {
            int rr = r + DR[d], cc = c + DC[d], run = 0;
            while(rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && b[rr * 8 + cc] == opp) {
                rr += DR[d];
                cc += DC[d];
                run++;
            }
            if(run > 0 && rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && b[rr * 8 + cc] == mark)
                total += run;
        }
        return total;
    }

    static bool reversiHasMove(const uint8_t* b, uint8_t mark) {
        for(int i = 0; i < 64; i++)
            if(b[i] == 0 && reversiFlips(b, i / 8, i % 8, mark) > 0) return true;
        return false;
    }

    void reversiMove(DuelMatch* m, uint8_t pid, int n, uint8_t mark) {
        if(n < 0 || n >= 64) return;
        int r = n / 8, c = n % 8;
        if(reversiFlips(m->board, r, c, mark) == 0) return; // illegal
        const int *DR = revDR(), *DC = revDC();
        uint8_t opp = (mark == 1) ? 2 : 1;
        m->board[n] = mark;
        for(int d = 0; d < 8; d++) {
            int rr = r + DR[d], cc = c + DC[d], run = 0;
            while(rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && m->board[rr * 8 + cc] == opp) {
                rr += DR[d];
                cc += DC[d];
                run++;
            }
            if(run > 0 && rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && m->board[rr * 8 + cc] == mark) {
                rr = r + DR[d];
                cc = c + DC[d];
                for(int s = 0; s < run; s++) {
                    m->board[rr * 8 + cc] = mark;
                    rr += DR[d];
                    cc += DC[d];
                }
            }
        }
        // Whose turn next: opponent if they can move, else same player if they can,
        // else the board is settled -> count discs and finish.
        uint8_t oppPid = (pid == m->a) ? m->b : m->a;
        if(reversiHasMove(m->board, opp))
            m->turn = oppPid;
        else if(reversiHasMove(m->board, mark))
            m->turn = pid; // opponent passes
        else
            reversiFinish(m);
    }

    void reversiFinish(DuelMatch* m) {
        int a = 0, b = 0;
        for(int i = 0; i < 64; i++) {
            if(m->board[i] == 1) a++;
            else if(m->board[i] == 2) b++;
        }
        uint8_t w = (a > b) ? m->a : (b > a) ? m->b : 0;
        duelFinish(m, w);
    }

    void gridMove(DuelMatch* m, uint8_t pid, int n, uint8_t mark) {
        int cols, rows, need;
        bool gravity;
        gridParams(m->kind, cols, rows, need, gravity);
        int row, col;
        if(gravity) {
            col = n;
            if(col < 0 || col >= cols) return;
            row = -1;
            for(int r = rows - 1; r >= 0; r--)
                if(m->board[r * cols + col] == 0) {
                    row = r;
                    break;
                }
            if(row < 0) return; // column full
        } else {
            if(n < 0 || n >= cols * rows) return;
            if(m->board[n] != 0) return; // cell taken
            row = n / cols;
            col = n % cols;
        }
        m->board[row * cols + col] = mark;
        if(gridWins(m->board, cols, rows, need, row, col, mark))
            duelFinish(m, pid);
        else if(gridFull(m->board, cols, rows))
            duelFinish(m, 0);
        else
            m->turn = (pid == m->a) ? m->b : m->a;
    }

    void dotsMove(DuelMatch* m, uint8_t pid, int n, uint8_t mark) {
        if(n < 0 || n >= DOTS_HEDGES + DOTS_VEDGES) return;
        if(n < DOTS_HEDGES) {
            if(m->hedges[n]) return;
            m->hedges[n] = 1;
        } else {
            int vi = n - DOTS_HEDGES;
            if(m->vedges[vi]) return;
            m->vedges[vi] = 1;
        }
        bool claimed = false;
        for(int r = 0; r < DOTS_H; r++)
            for(int c = 0; c < DOTS_W; c++) {
                int bi = r * DOTS_W + c;
                if(m->boxes[bi]) continue;
                if(dotsBoxComplete(m, r, c)) {
                    m->boxes[bi] = mark;
                    if(mark == 1)
                        m->sA++;
                    else
                        m->sB++;
                    claimed = true;
                }
            }
        if(m->sA + m->sB >= DOTS_BOXES) {
            uint8_t w = (m->sA > m->sB) ? m->a : (m->sB > m->sA) ? m->b : 0;
            duelFinish(m, w);
        } else if(!claimed) {
            m->turn = (pid == m->a) ? m->b : m->a; // completing a box grants another turn
        }
    }

    static bool dotsBoxComplete(const DuelMatch* m, int r, int c) {
        return m->hedges[r * DOTS_W + c] && m->hedges[(r + 1) * DOTS_W + c] &&
               m->vedges[r * (DOTS_W + 1) + c] && m->vedges[r * (DOTS_W + 1) + c + 1];
    }

    void duelFinish(DuelMatch* m, uint8_t winnerPid) {
        if(m->phase != 1) return;
        m->phase = 2;
        m->winner = winnerPid;
        uint8_t loser = (winnerPid == m->a) ? m->b : (winnerPid == m->b) ? m->a : 0;
        if(winnerPid) {
            awardScore(winnerPid, 300, "duelwin");
            hostEvent(HA_HOST_EVT_ROUND_WIN, winnerPid, loser);
        } else {
            hostEvent(HA_HOST_EVT_ROUND_DRAW, m->a, m->b);
        }
    }

    // A player returns to lobby or disconnects. Forfeit a live match to opponent.
    void duelOnLeave(uint8_t pid) {
        DuelMatch* m = matchOf(pid);
        if(!m) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->phase == 1) duelFinish(m, opp); // forfeit
        if(pid == m->a) m->aIn = false;
        if(pid == m->b) m->bIn = false;
        if(!m->aIn && !m->bIn) *m = DuelMatch{}; // both gone: free the slot
    }

    static bool gridFull(const uint8_t* b, int cols, int rows) {
        for(int i = 0; i < cols * rows; i++)
            if(b[i] == 0) return false;
        return true;
    }

    static bool
        gridWins(const uint8_t* b, int cols, int rows, int need, int row, int col, uint8_t disc) {
        static const int dr[4] = {0, 1, 1, 1};
        static const int dc[4] = {1, 0, 1, -1};
        for(int d = 0; d < 4; d++) {
            int cnt = 1;
            for(int s = 1; s < need; s++) {
                int r = row + dr[d] * s, c = col + dc[d] * s;
                if(r < 0 || r >= rows || c < 0 || c >= cols) break;
                if(b[r * cols + c] != disc) break;
                cnt++;
            }
            for(int s = 1; s < need; s++) {
                int r = row - dr[d] * s, c = col - dc[d] * s;
                if(r < 0 || r >= rows || c < 0 || c >= cols) break;
                if(b[r * cols + c] != disc) break;
                cnt++;
            }
            if(cnt >= need) return true;
        }
        return false;
    }

    String duelChallengesJson() {
        String s = "[";
        bool first = true;
        for(int i = 0; i < DUEL_MAX_CHALLENGES; i++) {
            if(!_c[i].used || _c[i].game != _active || !playerOnline(_c[i].from) ||
               !playerOnline(_c[i].to))
                continue;
            if(!first) s += ",";
            s += "{\"id\":";
            s += _c[i].id;
            s += ",\"from\":";
            s += _c[i].from;
            s += ",\"to\":";
            s += _c[i].to;
            s += "}";
            first = false;
        }
        s += "]";
        return s;
    }

    static String intArray(const uint8_t* a, int n) {
        String s = "[";
        for(int i = 0; i < n; i++) {
            if(i) s += ",";
            s += a[i];
        }
        s += "]";
        return s;
    }

    String duelJson(uint8_t pid) {
        DuelMatch* m = matchOf(pid);
        const char* kind = kindStr(_active);
        if(!m) {
            return String("{\"t\":\"duel\",\"kind\":\"") + kind +
                   "\",\"phase\":\"lobby\",\"challenges\":" + duelChallengesJson() + "}";
        }
        kind = kindStr(m->kind);
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        uint8_t me = (pid == m->a) ? 1 : 2;
        const char* phase = (m->phase == 2) ? "over" : "playing";
        bool paused = m->phase == 1 && !matchBothOnline(m->a, m->b);
        String s = String("{\"t\":\"duel\",\"kind\":\"") + kind + "\",\"phase\":\"" + phase +
                   "\",\"turn\":" + m->turn + ",\"me\":" + me + ",\"you\":" + pid + ",\"opp\":\"" +
                   ha_json_escape(_p[opp].nick) + "\"";
        s += ",\"paused\":";
        s += paused ? "true" : "false";
        if(m->kind == HA_GAME_DOTS) {
            s += ",\"w\":";
            s += DOTS_W;
            s += ",\"h\":";
            s += DOTS_H;
            s += ",\"hedges\":" + intArray(m->hedges, DOTS_HEDGES);
            s += ",\"vedges\":" + intArray(m->vedges, DOTS_VEDGES);
            s += ",\"boxes\":" + intArray(m->boxes, DOTS_BOXES);
            s += ",\"sme\":";
            s += (me == 1) ? m->sA : m->sB;
            s += ",\"sopp\":";
            s += (me == 1) ? m->sB : m->sA;
        } else if(m->kind == HA_GAME_REVERSI) {
            int cA = 0, cB = 0;
            for(int i = 0; i < 64; i++) {
                if(m->board[i] == 1) cA++;
                else if(m->board[i] == 2) cB++;
            }
            s += ",\"cols\":8,\"rows\":8";
            s += ",\"board\":" + intArray(m->board, 64);
            s += ",\"sme\":";
            s += (me == 1) ? cA : cB;
            s += ",\"sopp\":";
            s += (me == 1) ? cB : cA;
            // Legal moves for the player to move, so the client can highlight them.
            s += ",\"valid\":[";
            if(m->phase == 1 && !paused && m->turn == pid) {
                bool f = true;
                for(int i = 0; i < 64; i++)
                    if(m->board[i] == 0 && reversiFlips(m->board, i / 8, i % 8, me) > 0) {
                        if(!f) s += ",";
                        s += i;
                        f = false;
                    }
            }
            s += "]";
        } else {
            int cols, rows, need;
            bool gravity;
            gridParams(m->kind, cols, rows, need, gravity);
            s += ",\"cols\":";
            s += cols;
            s += ",\"rows\":";
            s += rows;
            s += ",\"need\":";
            s += need;
            s += ",\"gravity\":";
            s += gravity ? "true" : "false";
            s += ",\"board\":" + intArray(m->board, cols * rows);
        }
        if(m->phase == 2) {
            const char* r = (m->winner == 0) ? "draw" : (m->winner == pid) ? "win" : "lose";
            s += ",\"result\":\"";
            s += r;
            s += "\"";
        }
        s += "}";
        return s;
    }

    // ---------- drawing + guessing ----------
    // Reset round state only -- packs/packCount are content, streamed once at
    // session start, and must survive selectGame()/again clearing round state
    // (mirrors wyrClear/scrambleClear, which likewise leave their packs alone).
    void drawClear() {
        _d.phase = 0;
        _d.drawer = 0;
        _d.word[0] = '\0';
        _d.round = 0;
        _d.roundsTotal = 0;
        _d.deadline = 0;
        _d.revealUntil = 0;
        _d.winner = 0;
        _d.pack = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) _d.vote[i] = -1;
    }

    void drawAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_DRAW || _d.phase != 3) return;
        drawClear(); // back to idle; the tick restarts once 2+ players are present
        pushAll();
    }

    // Lobby / general chat: relay a player's line to everyone as a chat message.
    void onSay(uint8_t pid, const char* text) {
        if(!text[0]) return;
        haWsBroadcast(
            String("{\"t\":\"chat\",\"nick\":\"") + ha_json_escape(_p[pid].nick) + "\",\"text\":\"" +
            ha_json_escape(text) + "\"}");
        // Also surface it on the Flipper console so the host can follow the lobby chat.
        hostEvent(HA_HOST_EVT_CHAT, pid, 0, 0, text);
    }

    // Emoji reaction. Goes to whoever shares your screen: your opponent if you are
    // in a 1v1 match, otherwise everyone else who is also un-matched. In the lobby
    // and in every whole-group game nobody is in a match, so that second case is
    // "everyone" and the behaviour there is unchanged. Without this, six concurrent
    // duels spray emoji at each other about games nobody else can see.
    //
    // matchOf/pongMatchOf both gate on aIn/bIn, so a player who has returned to the
    // lobby from a finished match correctly counts as un-matched.
    //
    // The sender is always included: the client renders nothing locally and waits
    // for this echo, so dropping the sender would hide your own reaction from you.
    //
    // Uses type "emoji" so it never collides with the reaction-duel game's
    // {t:"react",phase} state messages.
    void onReact(uint8_t pid, const char* emoji) {
        if(!emoji[0]) return;
        String msg = String("{\"t\":\"emoji\",\"pid\":") + pid + ",\"nick\":\"" +
                     ha_json_escape(_p[pid].nick) + "\",\"avatar\":\"" +
                     ha_json_escape(_p[pid].avatar) + "\",\"emoji\":\"" +
                     ha_json_escape(emoji) + "\"}";
        DuelMatch* dm = isDuel(_active) ? matchOf(pid) : nullptr;
        PongMatch* pm = _active == HA_GAME_PONG ? pongMatchOf(pid) : nullptr;
        BattleMatch* bm = _active == HA_GAME_BATTLESHIP ? battleMatchOf(pid) : nullptr;
        ChessMatch* cm = _active == HA_GAME_CHESS ? chessMatchOf(pid) : nullptr;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!_p[i].used || !_p[i].wsId) continue;
            bool peer;
            if(dm)
                peer = (i == dm->a || i == dm->b);
            else if(pm)
                peer = (i == pm->a || i == pm->b);
            else if(bm)
                peer = (i == bm->a || i == bm->b);
            else if(cm)
                peer = (i == cm->a || i == cm->b);
            else
                peer = !inAnyMatch(i); // lobby / whole-group: reaches everyone not in a match
            if(peer) haWsSendWs(_p[i].wsId, msg);
        }
    }

    void drawShuffleDeck(uint8_t pack, uint8_t count) {
        if(pack >= TRIVIA_MAX_TOPICS || count == 0 || count > PACK_MAX_ITEMS) return;
        for(uint8_t i = 0; i < count; i++) _d.wordOrder[pack][i] = i;
        for(int i = count - 1; i > 0; i--) {
            int j = (int)random(i + 1);
            uint8_t t = _d.wordOrder[pack][i];
            _d.wordOrder[pack][i] = _d.wordOrder[pack][j];
            _d.wordOrder[pack][j] = t;
        }
        if(count > 1 && _d.wordOrder[pack][0] == _d.lastWord[pack]) {
            uint8_t t = _d.wordOrder[pack][0];
            _d.wordOrder[pack][0] = _d.wordOrder[pack][1];
            _d.wordOrder[pack][1] = t;
        }
    }

    void drawStart(uint32_t now) {
        if(_d.packCount == 0) return; // no pack streamed: refuse to start a round
        int used = connectedCount();
        if(used < 2) {
            _d.phase = 0;
            pushAll();
            return;
        }
        if(_d.round == 0) { // fresh game: everyone draws once (capped), scores reset
            _d.roundsTotal = used < 6 ? used : 6;
            if(_d.roundsTotal < 2) _d.roundsTotal = 2;
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(_p[i].used) _p[i].score = 0;
        }
        if(_d.round >= _d.roundsTotal) { // played them all -> final scoreboard
            _d.phase = 3;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        // Walk forward from the previous drawer. Keeping lastDrawer across
        // replays means a six-round cap cannot permanently starve higher PIDs.
        uint8_t drawer = 0;
        for(uint8_t step = 1; step <= HA_MAX_PLAYERS; step++) {
            uint8_t pid = (uint8_t)((_d.lastDrawer + step - 1) % HA_MAX_PLAYERS + 1);
            if(playerOnline(pid)) { drawer = pid; break; }
        }
        if(!drawer) {
            _d.phase = 0;
            return;
        }
        _d.lastDrawer = drawer;

        // Advance through every non-empty pack, instead of silently pinning
        // drawing to pack zero.
        bool packFound = false;
        for(uint8_t step = 0; step < _d.packCount; step++) {
            uint8_t pack = (uint8_t)((_d.nextPack + step) % _d.packCount);
            if(_d.packs[pack].count) {
                _d.pack = pack;
                _d.nextPack = (uint8_t)((pack + 1) % _d.packCount);
                packFound = true;
                break;
            }
        }
        if(!packFound) {
            _d.phase = 3;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        WordPack& dp = _d.packs[_d.pack];
        _d.drawer = drawer;
        uint8_t deckPos = (uint8_t)(_d.wordSeq[_d.pack] % dp.count);
        if(deckPos == 0) drawShuffleDeck(_d.pack, dp.count);
        uint8_t wordIndex = _d.wordOrder[_d.pack][deckPos];
        strlcpy(_d.word, dp.words[wordIndex].c_str(), sizeof(_d.word));
        _d.lastWord[_d.pack] = wordIndex;
        _d.wordSeq[_d.pack]++;
        _d.phase = 1;
        _d.round++;
        _d.winner = 0;
        _d.deadline = now + (uint32_t)DRAW_SECS * 1000;
        haWsBroadcast("{\"t\":\"ink\",\"clear\":true}");
        pushAll();
        hostEvent(HA_HOST_EVT_ROLE, drawer, 0, 0, "drawer");
    }

    void drawReveal(uint32_t now, uint8_t winner) {
        _d.phase = 2;
        _d.winner = winner;
        _d.revealUntil = now + DRAW_REVEAL_MS;
        pushAll();
    }

    void drawTick(uint32_t now) {
        if(_d.phase == 0) {
            if(connectedCount() >= 2) drawStart(now);
        } else if(_d.phase == 1) {
            if(!playerOnline(_d.drawer) || haTimeReached(now, _d.deadline)) drawReveal(now, 0);
        } else if(_d.phase == 2) {
            if(haTimeReached(now, _d.revealUntil)) drawStart(now);
        }
    }

    static bool wordMatch(const char* a, const char* b) {
        while(*a == ' ') a++;
        while(*b == ' ') b++;
        while(*a && *b) {
            char ca = *a, cb = *b;
            if(ca >= 'A' && ca <= 'Z') ca += 32;
            if(cb >= 'A' && cb <= 'Z') cb += 32;
            if(ca != cb) return false;
            a++;
            b++;
        }
        while(*a == ' ') a++;
        return *a == '\0' && *b == '\0';
    }

    void drawGuess(uint8_t pid, const char* text) {
        if(_active != HA_GAME_DRAW || _d.phase != 1 || pid == _d.drawer) return;
        if(wordMatch(text, _d.word)) {
            awardScore(pid, 200, "draw");
            if(playerOnline(_d.drawer)) {
                awardScore(_d.drawer, 100, "drawn");
            }
            hostEvent(HA_HOST_EVT_ROUND_WIN, pid, _d.drawer, 0, "guessed");
            drawReveal(clockNow(), pid);
        } else {
            haWsBroadcast(
                String("{\"t\":\"chat\",\"nick\":\"") + ha_json_escape(_p[pid].nick) +
                "\",\"text\":\"" + ha_json_escape(text) + "\"}");
        }
    }

    static bool jsonFloat01(const char* s, const char* key, char* out, size_t n) {
        const char* q = ha_json_find(s, key);
        if(!q) return false;
        char* end = nullptr;
        float value = strtof(q, &end);
        if(end == q || !isfinite(value) || value < 0.0f || value > 1.0f) return false;
        while(*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
        if(*end != ',' && *end != '}') return false;
        return snprintf(out, n, "%.4f", value) > 0;
    }

    // Relay the drawer's stroke to every other client as an "ink" message.
    void drawStroke(uint8_t pid, const char* json) {
        if(_active != HA_GAME_DRAW || _d.phase != 1 || pid != _d.drawer) return;
        String ink = "{\"t\":\"ink\"";
        static const char* keys[4] = {"x0", "y0", "x1", "y1"};
        char nums[4][16];
        for(int k = 0; k < 4; k++)
            if(!jsonFloat01(json, keys[k], nums[k], sizeof(nums[k]))) return;
        for(int k = 0; k < 4; k++) {
            ink += ",\"";
            ink += keys[k];
            ink += "\":";
            ink += nums[k];
        }
        ink += "}";
        for(uint8_t p = 1; p <= HA_MAX_PLAYERS; p++)
            if(_p[p].used && _p[p].wsId && p != _d.drawer) haWsSendWs(_p[p].wsId, ink);
    }

    void drawClearInk(uint8_t pid) {
        if(_active != HA_GAME_DRAW || pid != _d.drawer) return;
        for(uint8_t p = 1; p <= HA_MAX_PLAYERS; p++)
            if(_p[p].used && _p[p].wsId && p != _d.drawer)
                haWsSendWs(_p[p].wsId, "{\"t\":\"ink\",\"clear\":true}");
    }

    String drawJson(uint8_t pid) {
        if(_d.phase == 3) { // final scoreboard
            return String("{\"t\":\"draw\",\"phase\":\"final\",\"board\":") + triviaBoard() + "}";
        }
        String s = "{\"t\":\"draw\",\"phase\":\"";
        s += _d.phase == 1 ? "draw" : _d.phase == 2 ? "reveal" : "idle";
        s += "\",\"paused\":";
        s += _criticalPausePid ? "true" : "false";
        if(_d.phase != 0) {
            s += ",\"round\":";
            s += _d.round;
            s += ",\"rounds\":";
            s += _d.roundsTotal;
            s += ",\"pack\":\"";
            s += ha_json_escape(_d.packs[_d.pack].name.c_str());
            s += "\"";
            if(_d.phase == 2) {
                s += ",\"word\":\"";
                s += ha_json_escape(_d.word);
                s += "\",\"winner\":";
                if(_d.winner)
                    s += _d.winner;
                else
                    s += "null";
            } else {
                appendTimer(s, _d.deadline, (uint32_t)DRAW_SECS * 1000);
                if(pid == _d.drawer) {
                    s += ",\"role\":\"drawer\",\"word\":\"";
                    s += ha_json_escape(_d.word);
                    s += "\",\"drawer\":";
                    s += _d.drawer;
                } else {
                    s += ",\"role\":\"guesser\",\"len\":";
                    s += haUtf8Len(_d.word); // characters, not bytes: one blank per letter
                    s += ",\"drawer\":\"";
                    s += ha_json_escape(_p[_d.drawer].nick);
                    s += "\"";
                }
            }
        }
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }

    // ---------- pong ----------
    void pongClear() {
        memset(&_matches, 0, sizeof(_matches));
    }

    PongMatch* pongMatchOf(uint8_t pid) {
        for(int i = 0; i < PONG_MAX; i++) {
            if(!_matches.pong[i].used) continue;
            if(_matches.pong[i].a == pid && _matches.pong[i].aIn) return &_matches.pong[i];
            if(_matches.pong[i].b == pid && _matches.pong[i].bIn) return &_matches.pong[i];
        }
        return nullptr;
    }

    void pongServe(PongMatch* m, int dir) {
        m->bx = 0.5f;
        m->by = 0.5f;
        m->p1 = 0.5f;
        m->p2 = 0.5f;
        m->vx = dir > 0 ? 0.018f : -0.018f;
        m->vy = 0.010f;
        m->d1 = 0;
        m->d2 = 0;
    }

    void pongStart(PongMatch* m, uint8_t a, uint8_t b) {
        *m = PongMatch{};
        m->used = true;
        m->a = a;
        m->b = b;
        m->aIn = m->bIn = true;
        m->phase = 1;
        pongServe(m, 1);
    }

    void pongPaddle(uint8_t pid, int dir) {
        PongMatch* m = pongMatchOf(pid);
        if(!m || m->phase != 1 || !matchBothOnline(m->a, m->b)) return;
        if(dir < -1 || dir > 1) return;
        if(pid == m->a)
            m->d1 = (int8_t)dir;
        else
            m->d2 = (int8_t)dir;
    }

    void pongFinish(PongMatch* m, uint8_t winner) {
        if(m->phase != 1) return;
        m->phase = 2;
        m->winner = winner;
        uint8_t loser = (winner == m->a) ? m->b : m->a;
        awardScore(winner, 300, "pongwin");
        hostEvent(HA_HOST_EVT_ROUND_WIN, winner, loser);
    }

    void pongOnLeave(uint8_t pid) {
        PongMatch* m = pongMatchOf(pid);
        if(!m) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->phase == 1) pongFinish(m, opp);
        if(pid == m->a) m->aIn = false;
        if(pid == m->b) m->bIn = false;
        if(!m->aIn && !m->bIn) *m = PongMatch{};
    }

    void pongTick() {
        const float PADHALF = 0.11f, PSPEED = 0.03f;
        for(int i = 0; i < PONG_MAX; i++) {
            PongMatch* m = &_matches.pong[i];
            if(!m->used || m->phase != 1) continue;
            if(!matchBothOnline(m->a, m->b)) continue;
            m->p1 += m->d1 * PSPEED;
            m->p2 += m->d2 * PSPEED;
            if(m->p1 < PADHALF) m->p1 = PADHALF;
            if(m->p1 > 1 - PADHALF) m->p1 = 1 - PADHALF;
            if(m->p2 < PADHALF) m->p2 = PADHALF;
            if(m->p2 > 1 - PADHALF) m->p2 = 1 - PADHALF;
            m->bx += m->vx;
            m->by += m->vy;
            if(m->by < 0) {
                m->by = 0;
                m->vy = -m->vy;
            }
            if(m->by > 1) {
                m->by = 1;
                m->vy = -m->vy;
            }
            if(m->bx <= PONG_HIT_X) {
                if(fabsf(m->by - m->p1) <= PADHALF) {
                    m->bx = PONG_HIT_X;
                    m->vx = -m->vx;
                    m->vy += (m->by - m->p1) * 0.05f;
                } else {
                    m->s2++;
                    if(m->s2 >= PONG_WIN)
                        pongFinish(m, m->b);
                    else
                        pongServe(m, 1);
                }
            }
            if(m->phase == 1 && m->bx >= 1.0f - PONG_HIT_X) {
                if(fabsf(m->by - m->p2) <= PADHALF) {
                    m->bx = 1.0f - PONG_HIT_X;
                    m->vx = -m->vx;
                    m->vy += (m->by - m->p2) * 0.05f;
                } else {
                    m->s1++;
                    if(m->s1 >= PONG_WIN)
                        pongFinish(m, m->a);
                    else
                        pongServe(m, -1);
                }
            }
            if(_p[m->a].wsId) haWsSendWs(_p[m->a].wsId, pongJson(m->a));
            if(_p[m->b].wsId) haWsSendWs(_p[m->b].wsId, pongJson(m->b));
        }
    }

    static String pongF(float v) {
        int iv = (int)(v * 1000.0f + 0.5f);
        if(iv < 0) iv = 0;
        if(iv > 1000) iv = 1000;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d.%03d", iv / 1000, iv % 1000);
        return String(buf);
    }

    String pongJson(uint8_t pid) {
        PongMatch* m = pongMatchOf(pid);
        if(!m)
            return String("{\"t\":\"pong\",\"phase\":\"lobby\",\"challenges\":") +
                   duelChallengesJson() + "}";
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        uint8_t me = (pid == m->a) ? 1 : 2;
        String s = "{\"t\":\"pong\",\"phase\":\"";
        s += (m->phase == 2) ? "over" : "playing";
        s += "\",\"you\":";
        s += pid;
        s += ",\"paused\":";
        s += (m->phase == 1 && !matchBothOnline(m->a, m->b)) ? "true" : "false";
        s += ",\"me\":";
        s += me;
        s += ",\"opp\":\"";
        s += ha_json_escape(_p[opp].nick);
        s += "\",\"ball\":{\"x\":" + pongF(m->bx) + ",\"y\":" + pongF(m->by) + "}";
        s += ",\"p1\":" + pongF(m->p1) + ",\"p2\":" + pongF(m->p2);
        s += ",\"s1\":";
        s += m->s1;
        s += ",\"s2\":";
        s += m->s2;
        if(m->phase == 2) {
            const char* r = (m->winner == pid) ? "win" : "lose";
            s += ",\"result\":\"";
            s += r;
            s += "\"";
        }
        s += "}";
        return s;
    }

    // ================= whole-group party games ==================
    // Shared lobby/ready/countdown helpers on a Party sub-state.
    void partyClear(Party& pt) {
        pt.phase = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) pt.ready[i] = false;
        pt.round = 0;
        pt.roundsTotal = 0;
        pt.countdownEnd = 0;
        pt.lastSec = -1;
        pt.deadline = 0;
        pt.revealUntil = 0;
    }

    bool partyAllReady(const Party& pt) {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(!pt.ready[i]) return false;
        }
        return n >= 1;
    }

    // Players list with per-player ready flags, for the party lobby screens.
    String partyPlayersJson(const Party& pt) {
        String s = "[";
        bool first = true;
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
            if(!_p[pid].used) continue;
            if(!first) s += ",";
            first = false;
            s += "{\"pid\":";
            s += pid;
            s += ",\"nick\":\"";
            s += ha_json_escape(_p[pid].nick);
            s += "\",\"avatar\":\"";
            s += ha_json_escape(_p[pid].avatar);
            s += "\",\"ready\":";
            s += pt.ready[pid] ? "true" : "false";
            s += ",\"online\":";
            s += playerOnline(pid) ? "true" : "false";
            s += "}";
        }
        s += "]";
        return s;
    }

    // Broadcast pushAll once per second while a countdown ticks; returns true at zero.
    bool partyCountdownDone(Party& pt, uint32_t now) {
        if(haTimeReached(now, pt.countdownEnd)) return true;
        int sec = (int)((haTimeRemaining(now, pt.countdownEnd) + 999) / 1000);
        if(sec != pt.lastSec) {
            pt.lastSec = sec;
            pushAll();
        }
        return false;
    }

    int partyCountdownSec(const Party& pt) {
        uint32_t now = clockNow();
        if(haTimeReached(now, pt.countdownEnd)) return 0;
        return (int)((haTimeRemaining(now, pt.countdownEnd) + 999) / 1000);
    }

    void resetScoresAll() {
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used) _p[i].score = 0;
    }

    // A join/leave can complete a vote/round or cancel a pending start.
    void partyRosterChanged() {
        uint32_t now = clockNow();
        if(_active == HA_GAME_WYR) {
            wyrCheckStart();
            if(_wyr.pt.phase == 2 && wyrAllVoted()) wyrReveal(now);
        } else if(_active == HA_GAME_SCRAMBLE) {
            scrambleCheckStart();
            if(_scr.pt.phase == 2 && scrambleAllSolved()) scrambleReveal(now);
        } else if(_active == HA_GAME_REACT) {
            reactCheckStart();
            if(_react.pt.phase == 2 && reactAllResolved()) reactReveal(now);
        } else if(_active == HA_GAME_GUESSCOLOR) {
            gcCheckStart();
            if(_gc.pt.phase == 2 && gcAllGuessed()) gcReveal(now);
        } else if(_active == HA_GAME_SPECTRUM) {
            spectrumCheckStart();
            if(_spec.pt.phase == 2 && _spec.stage == 1 && spectrumAllGuessed())
                spectrumReveal(now);
        } else if(_active == HA_GAME_KMK) {
            kmkCheckStart();
            if(_kmk.pt.phase == 2 && _kmk.stage == 1 && kmkAllGuessed()) kmkReveal(now);
        }
    }

    // ---------- would you rather (live A/B poll) ----------
    // Which pack wins the pre-round vote, mirroring triviaWinningTopic(): most
    // votes wins, ties broken at random, and an untallied vote (total == 0)
    // picks uniformly at random among all packs. Guard packCount == 0 so an
    // empty game (no packs streamed yet) never indexes out of range.
    int wyrWinningPack() {
        if(_wyr.packCount == 0) return 0;
        int votes[TRIVIA_MAX_TOPICS] = {0};
        int total = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i) && _wyr.vote[i] >= 0 && _wyr.vote[i] < _wyr.packCount) {
                votes[_wyr.vote[i]]++;
                total++;
            }
        if(total == 0) return (int)random(_wyr.packCount);
        int best = 0;
        for(int i = 1; i < _wyr.packCount; i++)
            if(votes[i] > votes[best]) best = i;
        int tie[TRIVIA_MAX_TOPICS], tn = 0;
        for(int i = 0; i < _wyr.packCount; i++)
            if(votes[i] == votes[best]) tie[tn++] = i;
        return tie[(int)random(tn)];
    }

    void wyrClear() {
        partyClear(_wyr.pt);
        _wyr.prompt = 0;
        _wyr.pack = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _wyr.vote[i] = -1;
            _wyr.choice[i] = -1;
        }
    }

    void wyrReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_WYR) return;
        if(_wyr.pt.phase != 0 && _wyr.pt.phase != 4) return;
        if(_wyr.pt.phase == 4 && val) wyrClear(); // ready from the final screen -> new game
        _wyr.pt.ready[pid] = val;
        wyrCheckStart();
        pushAll();
    }

    void wyrVote(uint8_t pid, int pack) {
        if(_active != HA_GAME_WYR || _wyr.pt.phase != 0) return;
        if(pack < 0 || pack >= _wyr.packCount) return;
        _wyr.vote[pid] = (int8_t)pack;
        pushAll();
    }

    void wyrCheckStart() {
        Party& pt = _wyr.pt;
        if(pt.phase == 0 && _wyr.packCount > 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    bool wyrAllVoted() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(_wyr.choice[i] < 0) return false;
        }
        return n >= 1;
    }

    void wyrNextPrompt(uint32_t now) {
        Party& pt = _wyr.pt;
        if(pt.round >= WYR_ROUNDS) {
            pt.phase = 4; // final
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        WyrPack& pk = _wyr.packs[_wyr.pack];
        if(pk.count == 0) { // empty pack: nothing to play, end the game
            pt.phase = 4;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        _wyr.prompt = _wyr.promptSeq % pk.count;
        _wyr.promptSeq++;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) _wyr.choice[i] = -1;
        pt.phase = 2;
        pt.deadline = now + (uint32_t)WYR_VOTE_SECS * 1000;
        pushAll();
    }

    void wyrAnswer(uint8_t pid, int c) {
        if(_active != HA_GAME_WYR || _wyr.pt.phase != 2) return;
        if(c != 0 && c != 1) return;
        _wyr.choice[pid] = (int8_t)c;
        if(wyrAllVoted()) wyrReveal(clockNow());
        else pushAll();
    }

    void wyrReveal(uint32_t now) {
        _wyr.pt.phase = 3;
        _wyr.pt.revealUntil = now + WYR_REVEAL_MS;
        hostEvent(HA_HOST_EVT_ROUND_COMPLETE, 0, 0, _wyr.pt.round);
        pushAll();
    }

    void wyrAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_WYR || _wyr.pt.phase != 4) return;
        wyrClear();
        pushAll();
    }

    void wyrTick(uint32_t now) {
        Party& pt = _wyr.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                // Lock in the winning pack now (votes are frozen during the
                // countdown), mirroring trivia's topic lock.
                _wyr.pack = (uint8_t)wyrWinningPack();
                wyrNextPrompt(now);
            }
        } else if(pt.phase == 2) {
            if(haTimeReached(now, pt.deadline) || wyrAllVoted()) wyrReveal(now);
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) wyrNextPrompt(now);
        }
    }

    String wyrJson(uint8_t pid) {
        Party& pt = _wyr.pt;
        if(pt.phase == 0) {
            String s = String("{\"t\":\"wyr\",\"phase\":\"lobby\",\"you\":") + pid +
                       ",\"players\":" + partyPlayersJson(pt);
            s += ",\"packs\":[";
            int votes[TRIVIA_MAX_TOPICS] = {0};
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _wyr.vote[i] >= 0 && _wyr.vote[i] < _wyr.packCount)
                    votes[_wyr.vote[i]]++;
            for(int i = 0; i < _wyr.packCount; i++) {
                if(i) s += ",";
                s += "{\"name\":\"" + ha_json_escape(_wyr.packs[i].name.c_str()) + "\",\"votes\":" + votes[i] + "}";
            }
            s += "],\"myvote\":" + String((int)_wyr.vote[pid]);
            s += "}";
            return s;
        }
        if(pt.phase == 1)
            return String("{\"t\":\"wyr\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"wyr\",\"phase\":\"final\",\"you\":") + pid + "}";
        WyrPack& pk = _wyr.packs[_wyr.pack];
        const char* a = pk.items[_wyr.prompt].a.c_str();
        const char* b = pk.items[_wyr.prompt].b.c_str();
        int cA = 0, cB = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            if(_wyr.choice[i] == 0) cA++;
            else if(_wyr.choice[i] == 1) cB++;
        }
        String s = String("{\"t\":\"wyr\",\"phase\":\"") + (pt.phase == 3 ? "reveal" : "vote") +
                   "\",\"round\":" + pt.round + ",\"rounds\":" + WYR_ROUNDS + ",\"a\":\"" +
                   ha_json_escape(a) + "\",\"b\":\"" + ha_json_escape(b) + "\",\"myvote\":" +
                   _wyr.choice[pid] + ",\"counts\":[" + cA + "," + cB + "]";
        if(pt.phase == 2) { // asking: count down the vote window
            appendTimer(s, pt.deadline, (uint32_t)WYR_VOTE_SECS * 1000);
        } else if(pt.phase == 3) { // results: count down to the next prompt
            appendTimer(s, pt.revealUntil, WYR_REVEAL_MS);
        }
        s += "}";
        return s;
    }

    // ---------- word scramble race ----------
    // Which pack wins the pre-round vote, mirroring wyrWinningPack(): most votes
    // wins, ties broken at random, an untallied vote (total == 0) picks uniformly
    // at random among all packs. Guard packCount == 0 so an empty game never
    // indexes out of range.
    int scrambleWinningPack() {
        if(_scr.packCount == 0) return 0;
        int votes[TRIVIA_MAX_TOPICS] = {0};
        int total = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i) && _scr.vote[i] >= 0 && _scr.vote[i] < _scr.packCount) {
                votes[_scr.vote[i]]++;
                total++;
            }
        if(total == 0) return (int)random(_scr.packCount);
        int best = 0;
        for(int i = 1; i < _scr.packCount; i++)
            if(votes[i] > votes[best]) best = i;
        int tie[TRIVIA_MAX_TOPICS], tn = 0;
        for(int i = 0; i < _scr.packCount; i++)
            if(votes[i] == votes[best]) tie[tn++] = i;
        return tie[(int)random(tn)];
    }

    // Shuffle src into dst (NUL-terminated); retry a few times so it differs from src.
    // Shuffles by UTF-8 character, not byte: a multi-byte letter (an accented Latin
    // char, or any non-Latin script) moves as one glyph instead of splitting into
    // continuation bytes that render as garbage.
    void scrambleMake(char* dst, const char* src) {
        // Slice src into glyphs: offset + byte-length of each UTF-8 character.
        const char* off[24];
        uint8_t glen[24];
        int n = 0;
        for(const char* p = src; *p && n < 23;) {
            unsigned char c = (unsigned char)*p;
            int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            off[n] = p;
            glen[n] = (uint8_t)l;
            n++;
            p += l;
        }
        for(int attempt = 0; attempt < 8; attempt++) {
            int idx[24];
            for(int i = 0; i < n; i++) idx[i] = i;
            for(int i = n - 1; i > 0; i--) {
                int j = (int)(esp_random() % (uint32_t)(i + 1));
                int t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
            char* o = dst;
            for(int i = 0; i < n; i++) {
                memcpy(o, off[idx[i]], glen[idx[i]]);
                o += glen[idx[i]];
            }
            *o = '\0';
            if(n < 2 || strcmp(dst, src) != 0) return;
        }
    }

    void scrambleClear() {
        partyClear(_scr.pt);
        _scr.word[0] = '\0';
        _scr.scram[0] = '\0';
        _scr.solvedCount = 0;
        _scr.pack = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _scr.solved[i] = false;
            _scr.vote[i] = -1;
        }
    }

    void scrambleReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_SCRAMBLE) return;
        if(_scr.pt.phase != 0 && _scr.pt.phase != 4) return;
        if(_scr.pt.phase == 4 && val) scrambleClear();
        _scr.pt.ready[pid] = val;
        scrambleCheckStart();
        pushAll();
    }

    void scrambleVote(uint8_t pid, int pack) {
        if(_active != HA_GAME_SCRAMBLE || _scr.pt.phase != 0) return;
        if(pack < 0 || pack >= _scr.packCount) return;
        _scr.vote[pid] = (int8_t)pack;
        pushAll();
    }

    void scrambleCheckStart() {
        if(_scr.packCount == 0) return;
        Party& pt = _scr.pt;
        if(pt.phase == 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    bool scrambleAllSolved() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(!_scr.solved[i]) return false;
        }
        return n >= 1;
    }

    void scrambleNextWord(uint32_t now) {
        Party& pt = _scr.pt;
        if(pt.round >= SCR_ROUNDS) {
            pt.phase = 4;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        WordPack& p = _scr.packs[_scr.pack];
        if(p.count == 0) { // empty pack: nothing to play, end the game
            pt.phase = 4;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        strlcpy(_scr.word, p.words[_scr.wordSeq % p.count].c_str(), sizeof(_scr.word));
        _scr.wordSeq++;
        scrambleMake(_scr.scram, _scr.word);
        _scr.solvedCount = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) _scr.solved[i] = false;
        pt.phase = 2;
        pt.deadline = now + (uint32_t)SCR_SECS * 1000;
        pushAll();
    }

    void scrambleGuess(uint8_t pid, const char* text) {
        if(_active != HA_GAME_SCRAMBLE || _scr.pt.phase != 2) return;
        if(_scr.solved[pid]) return;
        if(!wordMatch(text, _scr.word)) return;
        _scr.solved[pid] = true;
        int pts = (_scr.solvedCount == 0) ? 200 :
                  (_scr.solvedCount == 1) ? 120 :
                  (_scr.solvedCount == 2) ? 80 :
                                            40;
        _scr.solvedCount++;
        awardScore(pid, pts, "scramble");
        haWsBroadcast(
            String("{\"t\":\"chat\",\"nick\":\"") + ha_json_escape(_p[pid].nick) +
            "\",\"text\":\"solved it!\"}");
        if(scrambleAllSolved()) scrambleReveal(clockNow());
        else pushAll();
    }

    void scrambleReveal(uint32_t now) {
        _scr.pt.phase = 3;
        _scr.pt.revealUntil = now + SCR_REVEAL_MS;
        pushAll();
    }

    void scrambleAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_SCRAMBLE || _scr.pt.phase != 4) return;
        scrambleClear();
        pushAll();
    }

    void scrambleTick(uint32_t now) {
        Party& pt = _scr.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                resetScoresAll();
                // Lock in the winning pack now (votes are frozen during the
                // countdown), mirroring WYR's pack lock.
                _scr.pack = (uint8_t)scrambleWinningPack();
                scrambleNextWord(now);
            }
        } else if(pt.phase == 2) {
            if(haTimeReached(now, pt.deadline) || scrambleAllSolved()) scrambleReveal(now);
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) scrambleNextWord(now);
        }
    }

    String scrambleJson(uint8_t pid) {
        Party& pt = _scr.pt;
        if(pt.phase == 0) {
            String s = String("{\"t\":\"scramble\",\"phase\":\"lobby\",\"you\":") + pid +
                       ",\"players\":" + partyPlayersJson(pt);
            s += ",\"packs\":[";
            int votes[TRIVIA_MAX_TOPICS] = {0};
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _scr.vote[i] >= 0 && _scr.vote[i] < _scr.packCount)
                    votes[_scr.vote[i]]++;
            for(int i = 0; i < _scr.packCount; i++) {
                if(i) s += ",";
                s += "{\"name\":\"" + ha_json_escape(_scr.packs[i].name.c_str()) + "\",\"votes\":" + votes[i] + "}";
            }
            s += "],\"myvote\":" + String((int)_scr.vote[pid]);
            s += "}";
            return s;
        }
        if(pt.phase == 1)
            return String("{\"t\":\"scramble\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"scramble\",\"phase\":\"final\",\"board\":") + triviaBoard() +
                   "}";
        String s = String("{\"t\":\"scramble\",\"phase\":\"") + (pt.phase == 3 ? "reveal" : "play") +
                   "\",\"round\":" + pt.round + ",\"rounds\":" + SCR_ROUNDS;
        if(pt.phase == 3) {
            s += ",\"word\":\"";
            s += ha_json_escape(_scr.word);
            s += "\"";
        } else {
            s += ",\"scram\":\"";
            s += ha_json_escape(_scr.scram);
            s += "\",\"len\":";
            s += (int)strlen(_scr.word);
            s += ",\"solved\":";
            s += _scr.solved[pid] ? "true" : "false";
            appendTimer(s, pt.deadline, (uint32_t)SCR_SECS * 1000);
        }
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }

    // ---------- reaction duel (fastest finger) ----------
    void reactClear() {
        partyClear(_react.pt);
        _react.goAt = 0;
        _react.goOn = false;
        _react.winner = 0;
        _react.winMs = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _react.tapped[i] = false;
            _react.dq[i] = false;
        }
    }

    void reactReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_REACT) return;
        if(_react.pt.phase != 0 && _react.pt.phase != 4) return;
        if(_react.pt.phase == 4 && val) reactClear();
        _react.pt.ready[pid] = val;
        reactCheckStart();
        pushAll();
    }

    void reactCheckStart() {
        Party& pt = _react.pt;
        if(pt.phase == 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    // Everyone has either tapped (green) or false-started (dq) -> round is settled.
    bool reactAllResolved() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(!_react.tapped[i] && !_react.dq[i]) return false;
        }
        return n >= 1;
    }

    void reactArm(uint32_t now) {
        Party& pt = _react.pt;
        if(pt.round >= REACT_ROUNDS) {
            pt.phase = 4;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        _react.goOn = false;
        _react.winner = 0;
        _react.winMs = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _react.tapped[i] = false;
            _react.dq[i] = false;
        }
        _react.goAt = now + 2000 + (esp_random() % 3000); // 2-5 s of red
        pt.phase = 2; // armed
        pushAll();
    }

    void reactTap(uint8_t pid) {
        if(_active != HA_GAME_REACT || _react.pt.phase != 2) return;
        if(_react.tapped[pid] || _react.dq[pid]) return;
        uint32_t now = clockNow();
        if(!haTimeReached(now, _react.goAt)) { // tapped while red -> false start
            _react.dq[pid] = true;
            if(reactAllResolved()) reactReveal(now);
            else pushAll();
            return;
        }
        _react.tapped[pid] = true;
        if(_react.winner == 0) {
            _react.winner = pid;
            _react.winMs = now - _react.goAt;
            awardScore(pid, 200, "react");
            reactReveal(now); // first valid tap ends the round
        } else {
            pushAll();
        }
    }

    void reactReveal(uint32_t now) {
        _react.pt.phase = 3;
        _react.pt.revealUntil = now + REACT_REVEAL_MS;
        pushAll();
    }

    void reactAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_REACT || _react.pt.phase != 4) return;
        reactClear();
        pushAll();
    }

    void reactTick(uint32_t now) {
        Party& pt = _react.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                resetScoresAll();
                reactArm(now);
            }
        } else if(pt.phase == 2) {
            if(!_react.goOn && haTimeReached(now, _react.goAt)) {
                _react.goOn = true; // red -> green: push so clients light up
                pushAll();
            }
            // nobody tapped for a while after green -> reveal with no winner
            if(_react.goOn && _react.winner == 0 && haTimeReached(now, _react.goAt + 6000))
                reactReveal(now);
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) reactArm(now);
        }
    }

    String reactJson(uint8_t pid) {
        Party& pt = _react.pt;
        if(pt.phase == 0)
            return String("{\"t\":\"react\",\"phase\":\"lobby\",\"you\":") + pid +
                   ",\"players\":" + partyPlayersJson(pt) + "}";
        if(pt.phase == 1)
            return String("{\"t\":\"react\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"react\",\"phase\":\"final\",\"board\":") + triviaBoard() + "}";
        if(pt.phase == 2) {
            String s = String("{\"t\":\"react\",\"phase\":\"armed\",\"round\":") + pt.round +
                       ",\"rounds\":" + REACT_ROUNDS + ",\"light\":\"" +
                       (_react.goOn ? "go" : "wait") + "\",\"dq\":" +
                       (_react.dq[pid] ? "true" : "false") + ",\"tapped\":" +
                       (_react.tapped[pid] ? "true" : "false") + ",\"scores\":" + playersJson() +
                       "}";
            return s;
        }
        // reveal
        String s = String("{\"t\":\"react\",\"phase\":\"reveal\",\"round\":") + pt.round +
                   ",\"rounds\":" + REACT_ROUNDS;
        if(_react.winner) {
            s += ",\"winner\":\"";
            s += ha_json_escape(_p[_react.winner].nick);
            s += "\",\"ms\":";
            s += _react.winMs;
            s += ",\"iwon\":";
            s += (_react.winner == pid) ? "true" : "false";
        } else {
            s += ",\"winner\":null";
        }
        s += ",\"dq\":";
        s += _react.dq[pid] ? "true" : "false";
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }

    // ---------- guess the color (closest RGB + speed) ----------
    void gcClear() {
        partyClear(_gc.pt);
        _gc.tr = _gc.tg = _gc.tb = 0;
        _gc.roundStart = 0;
        _gc.winner = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _gc.guessed[i] = false;
            _gc.gained[i] = 0;
            _gc.submitMs[i] = 0;
            _gc.gr[i] = _gc.gg[i] = _gc.gb[i] = 0;
        }
    }

    void gcReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_GUESSCOLOR) return;
        if(_gc.pt.phase != 0 && _gc.pt.phase != 4) return;
        if(_gc.pt.phase == 4 && val) gcClear();
        _gc.pt.ready[pid] = val;
        gcCheckStart();
        pushAll();
    }

    void gcCheckStart() {
        Party& pt = _gc.pt;
        if(pt.phase == 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    // Everyone present has submitted -> round is settled.
    bool gcAllGuessed() {
        int n = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            n++;
            if(!_gc.guessed[i]) return false;
        }
        return n >= 1;
    }

    void gcStartRound(uint32_t now) {
        Party& pt = _gc.pt;
        if(pt.round >= GC_ROUNDS) {
            pt.phase = 4;
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        _gc.tr = esp_random() % 256;
        _gc.tg = esp_random() % 256;
        _gc.tb = esp_random() % 256;
        _gc.winner = 0;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _gc.guessed[i] = false;
            _gc.gained[i] = 0;
            _gc.submitMs[i] = 0;
        }
        _gc.roundStart = now;
        pt.deadline = now + (uint32_t)GC_PLAY_SECS * 1000;
        pt.phase = 2;
        pushAll();
    }

    void gcGuess(uint8_t pid, int r, int g, int b) {
        if(_active != HA_GAME_GUESSCOLOR || _gc.pt.phase != 2) return;
        if(_gc.guessed[pid]) return;
        if(r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return;
        _gc.gr[pid] = (uint8_t)r;
        _gc.gg[pid] = (uint8_t)g;
        _gc.gb[pid] = (uint8_t)b;
        _gc.guessed[pid] = true;
        uint32_t now = clockNow();
        _gc.submitMs[pid] = now - _gc.roundStart;
        if(gcAllGuessed()) gcReveal(now);
        else pushAll();
    }

    void gcReveal(uint32_t now) {
        _gc.winner = 0;
        int bestPts = -1;
        uint32_t bestMs = 0xFFFFFFFF;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!_p[i].used) continue;
            if(!_gc.guessed[i]) {
                _gc.gained[i] = 0;
                continue;
            }
            int dr = (int)_gc.gr[i] - _gc.tr, dg = (int)_gc.gg[i] - _gc.tg,
                db = (int)_gc.gb[i] - _gc.tb;
            float dist = sqrtf((float)(dr * dr + dg * dg + db * db)); // 0..441.67
            int closeness = (int)(200.0f * (1.0f - dist / 441.673f) + 0.5f);
            if(closeness < 0) closeness = 0;
            float sf = 1.0f - (float)_gc.submitMs[i] / (float)GC_SPEED_MS;
            if(sf < 0) sf = 0;
            int speed = (int)(100.0f * sf + 0.5f);
            int pts = (int)((closeness + speed) / 30.0f + 0.5f); // rescale 0..300 -> 0..10
            if(pts > 10) pts = 10;
            _gc.gained[i] = pts;
            awardScore(i, pts, "gc");
            if(pts > bestPts || (pts == bestPts && _gc.submitMs[i] < bestMs)) {
                bestPts = pts;
                bestMs = _gc.submitMs[i];
                _gc.winner = i;
            }
        }
        _gc.pt.phase = 3;
        _gc.pt.revealUntil = now + GC_REVEAL_MS;
        pushAll();
    }

    void gcAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_GUESSCOLOR || _gc.pt.phase != 4) return;
        gcClear();
        pushAll();
    }

    void gcTick(uint32_t now) {
        Party& pt = _gc.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                resetScoresAll();
                gcStartRound(now);
            }
        } else if(pt.phase == 2) {
            if(haTimeReached(now, pt.deadline)) gcReveal(now);
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) gcStartRound(now);
        }
    }

    String gcJson(uint8_t pid) {
        Party& pt = _gc.pt;
        if(pt.phase == 0)
            return String("{\"t\":\"gc\",\"phase\":\"lobby\",\"you\":") + pid +
                   ",\"players\":" + partyPlayersJson(pt) + "}";
        if(pt.phase == 1)
            return String("{\"t\":\"gc\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"gc\",\"phase\":\"final\",\"board\":") + triviaBoard() + "}";
        char color[8];
        snprintf(color, sizeof(color), "#%02X%02X%02X", _gc.tr, _gc.tg, _gc.tb);
        if(pt.phase == 2) {
            String play = String("{\"t\":\"gc\",\"phase\":\"play\",\"round\":") + pt.round +
                          ",\"rounds\":" + GC_ROUNDS + ",\"color\":\"" + color +
                          "\",\"submitted\":" + (_gc.guessed[pid] ? "true" : "false");
            appendTimer(play, pt.deadline, (uint32_t)GC_PLAY_SECS * 1000);
            play += ",\"scores\":" + playersJson() + "}";
            return play;
        }
        // reveal
        String s = String("{\"t\":\"gc\",\"phase\":\"reveal\",\"round\":") + pt.round +
                   ",\"rounds\":" + GC_ROUNDS + ",\"r\":" + _gc.tr + ",\"g\":" + _gc.tg +
                   ",\"b\":" + _gc.tb + ",\"color\":\"" + color + "\"";
        // Every player's guess, so the reveal can compare them side by side.
        s += ",\"guesses\":[";
        bool gfirst = true;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!_p[i].used || !_gc.guessed[i]) continue;
            int dr = (int)_gc.gr[i] - _gc.tr, dg = (int)_gc.gg[i] - _gc.tg,
                db = (int)_gc.gb[i] - _gc.tb;
            int dist = (int)(sqrtf((float)(dr * dr + dg * dg + db * db)) + 0.5f);
            char gcol[8];
            snprintf(gcol, sizeof(gcol), "#%02X%02X%02X", _gc.gr[i], _gc.gg[i], _gc.gb[i]);
            if(!gfirst) s += ",";
            s += "{\"pid\":" + String(i) + ",\"nick\":\"" + ha_json_escape(_p[i].nick) +
                 "\",\"color\":\"" + gcol + "\",\"dist\":" + dist + ",\"points\":" + _gc.gained[i] + "}";
            gfirst = false;
        }
        s += "]";
        if(_gc.winner) {
            s += ",\"winner\":\"";
            s += ha_json_escape(_p[_gc.winner].nick);
            s += "\",\"winnerPid\":";
            s += _gc.winner;
            s += ",\"iwon\":";
            s += (_gc.winner == pid) ? "true" : "false";
        } else {
            s += ",\"winner\":null";
        }
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }

    // ---------- battleship (1v1, hidden fleets) ----------
    void battleClear() {
        memset(&_matches, 0, sizeof(_matches));
    }

    BattleMatch* battleMatchOf(uint8_t pid) {
        for(int i = 0; i < BATTLE_MAX; i++) {
            if(!_matches.battle[i].used) continue;
            if(_matches.battle[i].a == pid && _matches.battle[i].aIn) return &_matches.battle[i];
            if(_matches.battle[i].b == pid && _matches.battle[i].bIn) return &_matches.battle[i];
        }
        return nullptr;
    }

    void battleStart(BattleMatch* m, uint8_t a, uint8_t b, uint8_t first) {
        *m = BattleMatch{};
        m->used = true;
        m->a = a;
        m->b = b;
        m->aIn = m->bIn = true;
        m->phase = 0; // placement
        m->first = first;
        m->turn = first;
        m->winner = 0;
    }

    void battleFinish(BattleMatch* m, uint8_t winner) {
        if(!m || m->phase == 2) return;
        m->phase = 2;
        m->winner = winner;
        uint8_t loser = winner == m->a ? m->b : m->a;
        awardScore(winner, 300, "battlewin");
        hostEvent(HA_HOST_EVT_ROUND_WIN, winner, loser);
    }

    // Parse one base-10 int from `p`, advancing past it. Own parser (no strtol, which
    // the off-target Arduino shim doesn't provide).
    static bool bsReadInt(const char*& p, int& out) {
        while(*p == ' ') p++;
        bool neg = (*p == '-');
        if(neg) p++;
        if(*p < '0' || *p > '9') return false;
        uint32_t v = 0;
        const uint32_t limit = neg ? 2147483648UL : 2147483647UL;
        while(*p >= '0' && *p <= '9') {
            uint8_t digit = (uint8_t)(*p - '0');
            if(v > (limit - digit) / 10U) return false;
            v = v * 10U + digit;
            p++;
        }
        if(neg && v == 2147483648UL)
            out = (-2147483647 - 1);
        else
            out = neg ? -(int)v : (int)v;
        return true;
    }

    // ships is "r,c,d;r,c,d;..." in fixed ship order; d=0 horizontal, d=1 vertical.
    void battlePlace(uint8_t pid, const char* json) {
        BattleMatch* m = battleMatchOf(pid);
        if(!m || m->phase != 0 || !matchBothOnline(m->a, m->b)) return;
        char buf[96];
        if(!ha_json_str(json, "ships", buf, sizeof(buf))) return;
        uint8_t fleet[BS_N];
        memset(fleet, 0, sizeof(fleet));
        const char* p = buf;
        for(uint8_t s = 0; s < BS_SHIPS; s++) {
            int r, c, d;
            if(!bsReadInt(p, r)) return;
            if(*p++ != ',') return;
            if(!bsReadInt(p, c)) return;
            if(*p++ != ',') return;
            if(!bsReadInt(p, d)) return;
            if(d != 0 && d != 1) return;
            if(s + 1 < BS_SHIPS) {
                if(*p++ != ';') return;
            } else if(*p != '\0') {
                return;
            }
            for(uint8_t k = 0; k < BS_LEN[s]; k++) {
                int rr = r + (d ? (int)k : 0), cc = c + (d ? 0 : (int)k);
                if(rr < 0 || rr >= BS_SIZE || cc < 0 || cc >= BS_SIZE) return; // out of bounds
                int idx = rr * BS_SIZE + cc;
                if(fleet[idx]) return; // overlap
                fleet[idx] = s + 1; // ship id 1..BS_SHIPS
            }
        }
        uint8_t* myFleet = (pid == m->a) ? m->fleetA : m->fleetB;
        memcpy(myFleet, fleet, sizeof(fleet));
        if(pid == m->a)
            m->readyA = true;
        else
            m->readyB = true;
        if(m->readyA && m->readyB) {
            m->phase = 1; // both placed -> firing
            m->turn = m->first;
        }
        pushAll();
    }

    bool battleShipSunk(const uint8_t* fleet, const uint8_t* shot, uint8_t shipId) {
        for(int i = 0; i < BS_N; i++)
            if(fleet[i] == shipId && shot[i] != 2) return false;
        return true;
    }

    int battleShipsLeft(const uint8_t* fleet, const uint8_t* shot) {
        int left = 0;
        for(uint8_t s = 1; s <= BS_SHIPS; s++)
            if(!battleShipSunk(fleet, shot, s)) left++;
        return left;
    }

    void battleFire(uint8_t pid, int n) {
        BattleMatch* m = battleMatchOf(pid);
        if(!m || m->phase != 1 || m->turn != pid || !matchBothOnline(m->a, m->b)) return;
        if(n < 0 || n >= BS_N) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        uint8_t* oppFleet = (pid == m->a) ? m->fleetB : m->fleetA;
        uint8_t* shotOnOpp = (pid == m->a) ? m->shotOnB : m->shotOnA;
        if(shotOnOpp[n]) return; // already fired here
        bool hit = oppFleet[n] != 0;
        shotOnOpp[n] = hit ? 2 : 1;
        if(!hit) {
            m->turn = opp; // miss passes the turn
            pushAll();
            return;
        }
        if(pid == m->a)
            m->hitsA++;
        else
            m->hitsB++;
        uint8_t hits = (pid == m->a) ? m->hitsA : m->hitsB;
        if(battleShipSunk(oppFleet, shotOnOpp, oppFleet[n])) {
            const char* name = BS_NAMES[oppFleet[n] - 1];
            if(_p[pid].wsId)
                haWsSendWs(
                    _p[pid].wsId,
                    String("{\"t\":\"toast\",\"msg\":\"You sank their ") + name + "!\"}");
            if(_p[opp].wsId)
                haWsSendWs(
                    _p[opp].wsId,
                    String("{\"t\":\"toast\",\"msg\":\"Your ") + name + " was sunk!\"}");
        }
        if(hits >= BS_TOTAL) battleFinish(m, pid); // all enemy ships down
        // else: a hit keeps the turn (shoot again)
        pushAll();
    }

    void battleRematch(uint8_t pid) {
        BattleMatch* m = battleMatchOf(pid);
        if(!m || m->phase != 2) return;
        if(!matchBothOnline(m->a, m->b)) return;
        if(!m->aIn || !m->bIn) {
            if(_p[pid].wsId)
                haWsSendWs(_p[pid].wsId, String("{\"t\":\"toast\",\"msg\":\"Opponent left\"}"));
            battleOnLeave(pid);
            pushAll();
            return;
        }
        uint8_t next = (m->first == m->a) ? m->b : m->a; // alternate who fires first
        battleStart(m, m->a, m->b, next);
        pushAll();
    }

    void battleOnLeave(uint8_t pid) {
        BattleMatch* m = battleMatchOf(pid);
        if(!m) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->phase == 0 || m->phase == 1) battleFinish(m, opp); // forfeit
        if(pid == m->a) m->aIn = false;
        if(pid == m->b) m->bIn = false;
        if(!m->aIn && !m->bIn) *m = BattleMatch{}; // both gone: free the slot
    }

    String battleCells(const uint8_t* v) {
        String s = "[";
        for(int i = 0; i < BS_N; i++) {
            if(i) s += ",";
            s += v[i];
        }
        s += "]";
        return s;
    }

    String battleJson(uint8_t pid) {
        BattleMatch* m = battleMatchOf(pid);
        if(!m)
            return String("{\"t\":\"bs\",\"phase\":\"lobby\",\"challenges\":") +
                   duelChallengesJson() + "}";
        uint8_t me = (pid == m->a) ? 1 : 2;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->phase == 0) {
            bool ready = (pid == m->a) ? m->readyA : m->readyB;
            bool oppReady = (pid == m->a) ? m->readyB : m->readyA;
            bool paused = !matchBothOnline(m->a, m->b);
            return String("{\"t\":\"bs\",\"phase\":\"place\",\"you\":") + pid + ",\"me\":" + me +
                   ",\"opp\":\"" + ha_json_escape(_p[opp].nick) + "\",\"ready\":" +
                   (ready ? "true" : "false") + ",\"oppReady\":" +
                   (oppReady ? "true" : "false") + ",\"paused\":" +
                   (paused ? "true" : "false") + "}";
        }
        // firing / over: build the two grids from this player's perspective
        uint8_t* fleetSelf = (pid == m->a) ? m->fleetA : m->fleetB;
        uint8_t* shotOnSelf = (pid == m->a) ? m->shotOnA : m->shotOnB; // opponent's shots on me
        uint8_t* oppFleet = (pid == m->a) ? m->fleetB : m->fleetA;
        uint8_t* shotOnOpp = (pid == m->a) ? m->shotOnB : m->shotOnA; // my shots on them
        uint8_t mine[BS_N], track[BS_N];
        for(int i = 0; i < BS_N; i++) {
            uint8_t sh = shotOnSelf[i]; // 0 none, 1 miss, 2 hit
            mine[i] = (sh == 2) ? 3 : (sh == 1) ? 2 : (fleetSelf[i] ? 1 : 0);
            uint8_t st = shotOnOpp[i];
            // hidden info: only read oppFleet where I've already hit (st == 2)
            track[i] = (st == 2) ? (battleShipSunk(oppFleet, shotOnOpp, oppFleet[i]) ? 3 : 2) : st;
        }
        int myShips = battleShipsLeft(fleetSelf, shotOnSelf);
        int oppShips = battleShipsLeft(oppFleet, shotOnOpp);
        String s = "{\"t\":\"bs\",\"phase\":\"";
        s += (m->phase == 2) ? "over" : "fire";
        s += "\",\"you\":";
        s += pid;
        s += ",\"me\":";
        s += me;
        s += ",\"opp\":\"" + ha_json_escape(_p[opp].nick) + "\"";
        s += ",\"turn\":";
        s += m->turn;
        s += ",\"yourTurn\":";
        s += (m->turn == pid && matchBothOnline(m->a, m->b)) ? "true" : "false";
        s += ",\"paused\":";
        s += (m->phase != 2 && !matchBothOnline(m->a, m->b)) ? "true" : "false";
        s += ",\"myShips\":";
        s += myShips;
        s += ",\"oppShips\":";
        s += oppShips;
        s += ",\"mine\":" + battleCells(mine);
        s += ",\"track\":" + battleCells(track);
        if(m->phase == 2) {
            s += ",\"result\":\"";
            s += (m->winner == pid) ? "win" : "lose";
            s += "\",\"oppFleet\":" + battleCells(oppFleet); // reveal at game end
        }
        s += "}";
        return s;
    }

    // ---------- chess (rules core) ----------
    // Pure rules, no match state: everything below takes a ChessCore and is safe to
    // call on a scratch copy. Move generation is plain mailbox scanning: a few thousand
    // ops per move at human speed, which is nothing next to the WS traffic.

    static void chPush(uint16_t* out, int& n, int from, int to) {
        if(n < CH_MAX_MOVES) out[n++] = (uint16_t)(from * 64 + to);
    }

    // Is `sq` attacked by any piece of `bySide` (0 white, 1 black)?
    static bool chessAttacked(const ChessCore& c, int sq, uint8_t bySide) {
        int f = sq & 7, r = sq >> 3;
        uint8_t base = bySide ? 6 : 0; // white pieces are 1..6, black 7..12
        for(int i = 0; i < 8; i++) { // knights
            int ff = f + CH_NDF[i], rr = r + CH_NDR[i];
            if(ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
            if(c.sq[rr * 8 + ff] == base + 2) return true;
        }
        for(int i = 0; i < 8; i++) { // king
            int ff = f + CH_KDF[i], rr = r + CH_KDR[i];
            if(ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
            if(c.sq[rr * 8 + ff] == base + 6) return true;
        }
        // Pawns attack forwards, so an attacker stands one rank *behind* `sq`.
        int pr = bySide ? r + 1 : r - 1;
        if(pr >= 0 && pr <= 7) {
            if(f > 0 && c.sq[pr * 8 + f - 1] == base + 1) return true;
            if(f < 7 && c.sq[pr * 8 + f + 1] == base + 1) return true;
        }
        for(int d = 0; d < 8; d++) { // sliders: bishop/queen diagonally, rook/queen straight
            int ff = f + CH_SDF[d], rr = r + CH_SDR[d];
            while(ff >= 0 && ff <= 7 && rr >= 0 && rr <= 7) {
                uint8_t pc = c.sq[rr * 8 + ff];
                if(pc) {
                    uint8_t k = chKind(pc);
                    if(chSide(pc) == bySide && (k == 5 || k == (d < 4 ? 3 : 4))) return true;
                    break; // first piece on the ray blocks it
                }
                ff += CH_SDF[d];
                rr += CH_SDR[d];
            }
        }
        return false;
    }

    // Where is `side`'s king? -1 if it has none (only loaded test positions can).
    static int chessKingSq(const ChessCore& c, uint8_t side) {
        uint8_t king = side ? 12 : 6;
        for(int i = 0; i < 64; i++)
            if(c.sq[i] == king) return i;
        return -1;
    }

    static bool chessInCheck(const ChessCore& c) {
        int ks = chessKingSq(c, c.stm);
        return ks >= 0 && chessAttacked(c, ks, c.stm ^ 1);
    }

    // Every pseudo-legal move for c.stm (own king may end up attacked; chessGenLegal
    // filters that). Promotions emit ONE entry per from/to, because the promotion piece
    // is supplied at apply time and cannot change whether the move is legal.
    static int chessGenPseudo(const ChessCore& c, uint16_t* out) {
        int n = 0;
        uint8_t me = c.stm, opp = me ^ 1;
        for(int from = 0; from < 64; from++) {
            uint8_t pc = c.sq[from];
            if(!pc || chSide(pc) != me) continue;
            int f = from & 7, r = from >> 3;
            uint8_t k = chKind(pc);
            if(k == 1) { // pawn
                int dir = me ? -1 : 1, home = me ? 6 : 1, r1 = r + dir;
                if(r1 < 0 || r1 > 7) continue; // an unpromoted pawn on the last rank
                if(!c.sq[r1 * 8 + f]) {
                    chPush(out, n, from, r1 * 8 + f);
                    int r2 = r + 2 * dir; // double push needs both squares empty
                    if(r == home && !c.sq[r2 * 8 + f]) chPush(out, n, from, r2 * 8 + f);
                }
                for(int df = -1; df <= 1; df += 2) {
                    int ff = f + df;
                    if(ff < 0 || ff > 7) continue;
                    int to = r1 * 8 + ff;
                    uint8_t t = c.sq[to];
                    if(t) {
                        if(chSide(t) == opp) chPush(out, n, from, to);
                    } else if(to == (int)c.ep) {
                        chPush(out, n, from, to); // en passant
                    }
                }
            } else if(k == 2 || k == 6) { // knight, king: one step per direction
                const int8_t* sdf = (k == 2) ? CH_NDF : CH_KDF;
                const int8_t* sdr = (k == 2) ? CH_NDR : CH_KDR;
                for(int i = 0; i < 8; i++) {
                    int ff = f + sdf[i], rr = r + sdr[i];
                    if(ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
                    int to = rr * 8 + ff;
                    uint8_t t = c.sq[to];
                    if(t && chSide(t) == me) continue;
                    chPush(out, n, from, to);
                }
            } else { // bishop rays 0..3, rook rays 4..7, queen all eight
                int d0 = (k == 4) ? 4 : 0, d1 = (k == 3) ? 4 : 8;
                for(int d = d0; d < d1; d++) {
                    int ff = f + CH_SDF[d], rr = r + CH_SDR[d];
                    while(ff >= 0 && ff <= 7 && rr >= 0 && rr <= 7) {
                        int to = rr * 8 + ff;
                        uint8_t t = c.sq[to];
                        if(t && chSide(t) == me) break;
                        chPush(out, n, from, to);
                        if(t) break; // captured: the ray stops here
                        ff += CH_SDF[d];
                        rr += CH_SDR[d];
                    }
                }
            }
        }
        // Castling is validated in full here, so it never reaches the make/unmake
        // filter: right present, rook actually home (defensive, for loaded positions),
        // the path clear, and the king neither in check nor crossing an attacked square.
        int home = me ? 60 : 4, base = me ? 56 : 0;
        uint8_t rook = me ? 10 : 4, bitK = me ? 4 : 1, bitQ = me ? 8 : 2;
        if(c.sq[home] == (uint8_t)(me ? 12 : 6) && (c.rights & (bitK | bitQ)) &&
           !chessAttacked(c, home, opp)) {
            if((c.rights & bitK) && c.sq[base + 7] == rook && !c.sq[base + 5] &&
               !c.sq[base + 6] && !chessAttacked(c, base + 5, opp) &&
               !chessAttacked(c, base + 6, opp))
                chPush(out, n, home, base + 6);
            if((c.rights & bitQ) && c.sq[base] == rook && !c.sq[base + 1] &&
               !c.sq[base + 2] && !c.sq[base + 3] && !chessAttacked(c, base + 3, opp) &&
               !chessAttacked(c, base + 2, opp))
                chPush(out, n, home, base + 2); // b1/b8 may be attacked, only crossed by the rook
        }
        return n;
    }

    // Apply a move and record what chessUnmake needs. `promo` is a WHITE piece code
    // (2 = N, 3 = B, 4 = R, 5 = Q) and is only meaningful for a pawn reaching the last
    // rank; callers pass 0 otherwise, which chessUnmake relies on. Returns true when
    // the move was irreversible (pawn move or capture), on which the caller resets the
    // halfmove clock and the repetition history.
    static bool chessMake(ChessCore& c, int from, int to, uint8_t promo, ChessUndo& u) {
        uint8_t pc = c.sq[from], k = chKind(pc), me = chSide(pc);
        u.rights = c.rights;
        u.ep = c.ep;
        u.capSq = (uint8_t)to;
        u.captured = c.sq[to];
        bool irreversible = (k == 1) || (u.captured != 0);
        if(k == 1 && to == (int)c.ep && !u.captured) { // en passant: victim sits beside `to`
            u.capSq = (uint8_t)(me ? to + 8 : to - 8);
            u.captured = c.sq[u.capSq];
            c.sq[u.capSq] = 0;
        }
        c.sq[to] = pc;
        c.sq[from] = 0;
        if(k == 1 && (to >> 3) == (me ? 0 : 7) && promo >= 2 && promo <= 5)
            c.sq[to] = (uint8_t)(promo + (me ? 6 : 0));
        if(k == 6 && from == (me ? 60 : 4)) { // castling also hops the rook over the king
            if(to == from + 2) {
                c.sq[from + 1] = c.sq[from + 3];
                c.sq[from + 3] = 0;
            } else if(to == from - 2) {
                c.sq[from - 1] = c.sq[from - 4];
                c.sq[from - 4] = 0;
            }
        }
        if(k == 6) c.rights &= (uint8_t)~(me ? 0x0C : 0x03); // king moved: both rights gone
        c.rights &= (uint8_t)~chCornerBit(from);
        if(u.captured) c.rights &= (uint8_t)~chCornerBit(u.capSq);
        c.ep = (k == 1 && (to - from == 16 || from - to == 16)) ? (int8_t)((from + to) / 2) : -1;
        c.stm ^= 1;
        return irreversible;
    }

    // Exact inverse of chessMake, given the same from/to/promo and its ChessUndo.
    static void chessUnmake(ChessCore& c, int from, int to, uint8_t promo,
                            const ChessUndo& u) {
        c.stm ^= 1; // back to the side that moved
        uint8_t me = c.stm, pc = c.sq[to];
        if(promo >= 2 && promo <= 5) pc = me ? 7 : 1; // demote: only a pawn could promote
        c.sq[from] = pc;
        c.sq[to] = 0;
        if(u.captured) c.sq[u.capSq] = u.captured; // capSq != to for en passant
        if(chKind(pc) == 6 && from == (me ? 60 : 4)) { // un-hop the castling rook
            if(to == from + 2) {
                c.sq[from + 3] = c.sq[from + 1];
                c.sq[from + 1] = 0;
            } else if(to == from - 2) {
                c.sq[from - 4] = c.sq[from - 1];
                c.sq[from - 1] = 0;
            }
        }
        c.rights = u.rights;
        c.ep = u.ep;
    }

    // Pseudo-legal moves that do not leave the mover's own king attacked. Pins and
    // en-passant discovered checks fall out of the make/test/unmake for free.
    static int chessGenLegal(const ChessCore& c, uint16_t* out) {
        uint16_t ps[CH_MAX_MOVES];
        int np = chessGenPseudo(c, ps), n = 0;
        ChessCore w = c;
        uint8_t me = c.stm;
        for(int i = 0; i < np; i++) {
            int from = ps[i] >> 6, to = ps[i] & 63;
            ChessUndo u;
            chessMake(w, from, to, 0, u);
            int ks = chessKingSq(w, me);
            if(ks < 0 || !chessAttacked(w, ks, me ^ 1)) out[n++] = ps[i];
            chessUnmake(w, from, to, 0, u);
        }
        return n;
    }

    // FIDE 6.9: on a flag fall the opponent only wins if they *could* mate by some
    // series of legal moves (the helpmate test, not "can force"). So K+N vs K+N is a
    // win on time, but K+N vs a bare king is a draw.
    static bool chessCanMateVs(const ChessCore& c, uint8_t side) {
        int minors = 0, heavy = 0, oppPieces = 0;
        for(int i = 0; i < 64; i++) {
            uint8_t pc = c.sq[i];
            if(!pc || chKind(pc) == 6) continue;
            if(chSide(pc) != side)
                oppPieces++;
            else if(chKind(pc) == 2 || chKind(pc) == 3)
                minors++;
            else
                heavy++; // pawn, rook or queen: a mate always exists
        }
        if(heavy) return true;
        if(minors == 0) return false; // bare king
        return !(minors == 1 && oppPieces == 0); // lone minor vs lone king cannot mate
    }

    // Dead position (FIDE 5.2.2): no legal sequence at all reaches a mate, so the game
    // is drawn the instant it arises. The standard material subset: K vs K, K+minor vs
    // K, and any number of bishops as long as they all stand on one square color.
    // K+N vs K+N is NOT dead: it has helpmates.
    static bool chessDeadPosition(const ChessCore& c) {
        int knights = 0, bishops = 0, color = -1;
        for(int i = 0; i < 64; i++) {
            uint8_t pc = c.sq[i];
            if(!pc) continue;
            uint8_t k = chKind(pc);
            if(k == 6) continue;
            if(k == 2) {
                knights++;
            } else if(k == 3) {
                int sc = (((i >> 3) + (i & 7)) & 1);
                if(color < 0)
                    color = sc;
                else if(color != sc)
                    return false; // bishops on both colors can mate
                bishops++;
            } else {
                return false; // pawn, rook or queen
            }
        }
        if(knights + bishops <= 1) return true; // K vs K, or a single minor
        return knights == 0; // bishops only, and the loop proved they share a color
    }

    // Can the side to move actually capture en passant here? FIDE 9.2 compares the
    // *possible moves*, not the bare ep square, so a hash that always folds in the ep
    // file reports two identical positions as different and repetition never triggers.
    static bool chessEpLegal(const ChessCore& c) {
        if(c.ep < 0) return false;
        int to = c.ep, f = to & 7, pr = (to >> 3) + (c.stm ? 1 : -1);
        if(pr < 0 || pr > 7) return false;
        uint8_t pawn = c.stm ? 7 : 1;
        ChessCore w = c;
        for(int df = -1; df <= 1; df += 2) {
            int ff = f + df;
            if(ff < 0 || ff > 7) continue;
            int from = pr * 8 + ff;
            if(w.sq[from] != pawn) continue;
            ChessUndo u;
            chessMake(w, from, to, 0, u);
            int ks = chessKingSq(w, c.stm);
            bool ok = (ks < 0) || !chessAttacked(w, ks, c.stm ^ 1);
            chessUnmake(w, from, to, 0, u);
            if(ok) return true;
        }
        return false;
    }

    // Position key for repetition detection, recomputed from scratch (a 64-square scan
    // once per move; incremental updating would buy nothing at this rate).
    static uint32_t chessHash(const ChessCore& c) {
        uint32_t h = 0;
        for(int i = 0; i < 64; i++)
            if(c.sq[i]) h ^= chessZobKey((c.sq[i] - 1) * 64 + i);
        if(c.stm) h ^= chessZobKey(768);
        h ^= chessZobKey(769 + (c.rights & 15));
        if(chessEpLegal(c)) h ^= chessZobKey(785 + (c.ep & 7));
        return h;
    }

#ifdef HA_CHESS_TEST
public:
    // Move-path enumeration, the standard yardstick for a move generator: the number
    // of distinct legal move sequences of the given length. Test builds only.
    static uint32_t chessPerft(ChessCore& c, int depth) {
        if(depth <= 0) return 1;
        uint16_t mv[CH_MAX_MOVES];
        int n = chessGenLegal(c, mv);
        uint32_t total = 0;
        for(int i = 0; i < n; i++) {
            int from = mv[i] >> 6, to = mv[i] & 63;
            ChessUndo u;
            // Perft counts each promotion piece as its own move, so expand the single
            // entry our encoding generates back into the four choices.
            if(chKind(c.sq[from]) == 1 && (to >> 3) == (c.stm ? 0 : 7)) {
                for(uint8_t p = 2; p <= 5; p++) {
                    chessMake(c, from, to, p, u);
                    total += chessPerft(c, depth - 1);
                    chessUnmake(c, from, to, p, u);
                }
            } else {
                chessMake(c, from, to, 0, u);
                total += chessPerft(c, depth - 1);
                chessUnmake(c, from, to, 0, u);
            }
        }
        return total;
    }

    // board64 is exactly 64 chars, index 0 = a1 .. 63 = h8, FEN letters (uppercase =
    // white) and '.' for an empty square. False on a bad length or character.
    static bool chessLoadCore(ChessCore& c, const char* board64, uint8_t stm,
                              uint8_t rights, int8_t ep) {
        if(!board64 || strlen(board64) != 64) return false;
        const char* codes = "PNBRQKpnbrqk";
        ChessCore t = ChessCore{};
        for(int i = 0; i < 64; i++) {
            if(board64[i] == '.') continue;
            const char* p = strchr(codes, board64[i]);
            if(!p) return false;
            t.sq[i] = (uint8_t)(p - codes + 1);
        }
        t.stm = stm ? 1 : 0;
        t.rights = rights & 15;
        t.ep = ep;
        c = t;
        return true;
    }
private:
#endif

    // ---------- chess (match) ----------
    // Lifecycle, clocks and serialization around the rules core above. Same shape as
    // battleship: one slot per live pairing, freed when both players have detached.
    void chessClear() {
        memset(&_matches, 0, sizeof(_matches));
    }

    ChessMatch* chessMatchOf(uint8_t pid) {
        for(int i = 0; i < CHESS_MAX; i++) {
            if(!_matches.chess[i].used) continue;
            if(_matches.chess[i].a == pid && _matches.chess[i].aIn) return &_matches.chess[i];
            if(_matches.chess[i].b == pid && _matches.chess[i].bIn) return &_matches.chess[i];
        }
        return nullptr;
    }

    // Colors are per game, not per seat: a rematch swaps them, so every side/pid
    // translation goes through m->white rather than through a/b.
    static uint8_t chessSideOf(const ChessMatch* m, uint8_t pid) {
        return pid == m->white ? 0 : 1;
    }
    static uint8_t chessPidOf(const ChessMatch* m, uint8_t side) {
        return side ? ((m->white == m->a) ? m->b : m->a) : m->white;
    }
    static uint8_t chessTurnPid(const ChessMatch* m) { return chessPidOf(m, m->core.stm); }

    void chessStart(ChessMatch* m, uint8_t a, uint8_t b, uint8_t whitePid) {
        *m = ChessMatch{};
        m->used = true;
        m->a = a;
        m->b = b;
        m->aIn = m->bIn = true;
        m->white = whitePid;
        m->phase = 1;
        m->winner = 0;
        static const uint8_t back[8] = {4, 2, 3, 5, 6, 3, 2, 4}; // R N B Q K B N R
        for(int f = 0; f < 8; f++) {
            m->core.sq[f] = back[f]; // a1..h1
            m->core.sq[8 + f] = 1; // white pawns
            m->core.sq[48 + f] = 7; // black pawns
            m->core.sq[56 + f] = (uint8_t)(back[f] + 6); // a8..h8
        }
        m->core.stm = 0;
        m->core.rights = 15;
        m->core.ep = -1;
        m->halfmove = 0;
        m->fullmove = 1;
        m->clockMs[0] = m->clockMs[1] = CH_CLOCK_MS;
        m->lastStamp = clockNow();
        m->lastMove = -1;
        m->offerBy = 0;
        m->hist[0] = chessHash(m->core);
        m->histLen = 1;
    }

    // Scoring copies duelFinish (battleship's finish forgot it): 300 to the winner,
    // nothing on a draw, and the result goes up the UART either way. winnerPid 0 = draw.
    void chessFinish(ChessMatch* m, uint8_t winnerPid, uint8_t reason) {
        if(m->phase != 1) return;
        m->phase = 2;
        m->winner = winnerPid;
        m->reason = reason;
        uint8_t loser = (winnerPid == m->a) ? m->b : (winnerPid == m->b) ? m->a : 0;
        if(winnerPid) {
            awardScore(winnerPid, 300, "chesswin");
            hostEvent(HA_HOST_EVT_ROUND_WIN, winnerPid, loser, reason, chessReasonStr(reason));
        } else {
            hostEvent(HA_HOST_EVT_ROUND_DRAW, m->a, m->b, reason, chessReasonStr(reason));
        }
    }

    // The flag falls for the side to move. FIDE 6.9: the opponent only wins if they
    // could still mate by SOME legal sequence, otherwise the game is drawn.
    void chessFlagFall(ChessMatch* m) {
        uint8_t side = m->core.stm, opp = (uint8_t)(side ^ 1);
        m->clockMs[side] = 0;
        if(chessCanMateVs(m->core, opp))
            chessFinish(m, chessPidOf(m, opp), CH_R_FLAG);
        else
            chessFinish(m, 0, CH_R_FLAGDRAW);
    }

    // How often the position now on the board has occurred, counting this occurrence.
    static int chessRepCount(const ChessMatch* m) {
        uint32_t h = chessHash(m->core);
        int n = 0;
        for(uint16_t i = 0; i < m->histLen; i++)
            if(m->hist[i] == h) n++;
        return n;
    }

    void chessMove(uint8_t pid, int from, int to, int promo) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m || m->phase != 1 || m->clockPaused || !matchBothOnline(m->a, m->b) ||
           chessTurnPid(m) != pid)
            return;
        uint8_t stm = m->core.stm;
        uint32_t now = clockNow(), elapsed = now - m->lastStamp;
        if(elapsed >= m->clockMs[stm]) { // the move arrived after the flag fell: ignore it
            chessFlagFall(m);
            pushAll();
            return;
        }
        if(from < 0 || from > 63 || to < 0 || to > 63) return;
        // Never hand chessMake a move it did not generate: its castling branch hops the
        // rook on any e1-g1/c1 king move without re-checking, so a spoofed one corrupts
        // the board.
        uint16_t mv[CH_MAX_MOVES];
        int n = chessGenLegal(m->core, mv);
        bool legal = false;
        for(int i = 0; i < n; i++)
            if(mv[i] == (uint16_t)(from * 64 + to)) legal = true;
        if(!legal) return;
        // A pawn reaching the last rank must name a promotion piece, and nothing else may.
        bool isPromo = chKind(m->core.sq[from]) == 1 && (to >> 3) == (stm ? 0 : 7);
        if(isPromo != (promo >= 2 && promo <= 5)) return;

        m->clockMs[stm] -= elapsed;
        m->lastStamp = now;
        ChessUndo u;
        bool irrev = chessMake(m->core, from, to, isPromo ? (uint8_t)promo : 0, u);
        m->lastMove = (int16_t)(from * 64 + to);
        m->offerBy = 0; // a pending draw offer lapses once a move is played
        if(stm) m->fullmove++;
        if(irrev) { // a pawn move or capture can never repeat: the record starts over
            m->halfmove = 0;
            m->histLen = 0;
        } else {
            m->halfmove++;
        }
        if(m->histLen < CH_HIST) m->hist[m->histLen++] = chessHash(m->core);

        // Checkmate outranks the automatic counters (FIDE 9.6.2): a mating move ends the
        // game even when it also completes the 75-move or fivefold count.
        uint16_t reply[CH_MAX_MOVES];
        bool stuck = chessGenLegal(m->core, reply) == 0; // no reply: mate or stalemate
        if(stuck && chessInCheck(m->core))
            chessFinish(m, pid, CH_R_MATE);
        else if(stuck)
            chessFinish(m, 0, CH_R_STALEMATE);
        else if(chessDeadPosition(m->core))
            chessFinish(m, 0, CH_R_MATERIAL);
        else if(m->halfmove >= 150)
            chessFinish(m, 0, CH_R_MOVE75);
        else if(chessRepCount(m) >= 5)
            chessFinish(m, 0, CH_R_REP5);
        pushAll();
    }

    void chessResign(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m || m->phase != 1 || m->clockPaused || !matchBothOnline(m->a, m->b)) return;
        chessFinish(m, (pid == m->a) ? m->b : m->a, CH_R_RESIGN);
        pushAll();
    }

    // Offer a draw, or accept the one already on the table.
    void chessDraw(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m || m->phase != 1 || m->clockPaused || !matchBothOnline(m->a, m->b)) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->offerBy == pid) return;
        if(m->offerBy == opp) {
            chessFinish(m, 0, CH_R_AGREE);
        } else {
            m->offerBy = pid;
            if(_p[opp].wsId)
                haWsSendWs(
                    _p[opp].wsId,
                    String("{\"t\":\"toast\",\"msg\":\"") + ha_json_escape(_p[pid].nick) +
                        " offers a draw\"}");
        }
        pushAll();
    }

    // Threefold and the 50-move rule are claims, not automatic: only the player to move
    // may make them, and only while the count actually stands.
    void chessClaim(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m || m->phase != 1 || m->clockPaused || !matchBothOnline(m->a, m->b) ||
           chessTurnPid(m) != pid)
            return;
        if(chessRepCount(m) >= 3)
            chessFinish(m, 0, CH_R_REP3);
        else if(m->halfmove >= 100)
            chessFinish(m, 0, CH_R_MOVE50);
        else
            return; // nothing to claim
        pushAll();
    }

    void chessRematch(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m || m->phase != 2) return;
        if(!matchBothOnline(m->a, m->b)) return;
        if(!m->aIn || !m->bIn) {
            if(_p[pid].wsId)
                haWsSendWs(_p[pid].wsId, String("{\"t\":\"toast\",\"msg\":\"Opponent left\"}"));
            chessOnLeave(pid);
            pushAll();
            return;
        }
        uint8_t next = (m->white == m->a) ? m->b : m->a; // colors swap every game
        chessStart(m, m->a, m->b, next);
        pushAll();
    }

    void chessOnLeave(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m) return;
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        if(m->phase == 1) chessFinish(m, opp, CH_R_LEFT); // forfeit
        if(pid == m->a) m->aIn = false;
        if(pid == m->b) m->bIn = false;
        if(!m->aIn && !m->bIn) *m = ChessMatch{}; // both gone: free the slot
    }

    // The only game whose state changes with no input at all. Nothing is pushed unless a
    // flag actually fell: phones count down from the relative remaining value they received.
    void chessTick(uint32_t now) {
        bool ended = false;
        for(int i = 0; i < CHESS_MAX; i++) {
            ChessMatch* m = &_matches.chess[i];
            if(!m->used || m->phase != 1) continue;
            if(m->clockPaused || !matchBothOnline(m->a, m->b)) continue;
            if((now - m->lastStamp) < m->clockMs[m->core.stm]) continue;
            chessFlagFall(m);
            ended = true;
        }
        if(ended) pushAll();
    }

    // 64 chars, index 0 = a1: FEN letters, uppercase white, '.' empty.
    static String chessBoardStr(const ChessCore& c) {
        char b[65];
        for(int i = 0; i < 64; i++) b[i] = ".PNBRQKpnbrqk"[c.sq[i]];
        b[64] = '\0';
        return String(b);
    }

    static const char* chessReasonStr(uint8_t r) {
        switch(r) {
        case CH_R_MATE:
            return "mate";
        case CH_R_STALEMATE:
            return "stalemate";
        case CH_R_RESIGN:
            return "resign";
        case CH_R_FLAG:
            return "flag";
        case CH_R_FLAGDRAW:
            return "flagdraw";
        case CH_R_MATERIAL:
            return "material";
        case CH_R_REP3:
            return "rep3";
        case CH_R_REP5:
            return "rep5";
        case CH_R_MOVE50:
            return "move50";
        case CH_R_MOVE75:
            return "move75";
        case CH_R_AGREE:
            return "agree";
        case CH_R_LEFT:
            return "left";
        }
        return "";
    }

    String chessJson(uint8_t pid) {
        ChessMatch* m = chessMatchOf(pid);
        if(!m)
            return String("{\"t\":\"chess\",\"phase\":\"lobby\",\"challenges\":") +
                   duelChallengesJson() + "}";
        uint8_t opp = (pid == m->a) ? m->b : m->a;
        uint8_t stm = m->core.stm, turn = chessTurnPid(m);
        bool paused = m->phase == 1 && (m->clockPaused || !matchBothOnline(m->a, m->b));
        bool yourTurn = (turn == pid) && !paused;
        // One clock reading for the whole message, so the two relative clocks agree. The
        // running clock freezes once the game is over -- the over screen is not a place
        // to watch time tick away.
        uint32_t now = clockNow(), rem = m->clockMs[stm];
        if(m->phase == 1 && !m->clockPaused) {
            uint32_t spent = now - m->lastStamp;
            rem -= (spent < rem) ? spent : rem;
        }
        String s = "{\"t\":\"chess\",\"phase\":\"";
        s += (m->phase == 2) ? "over" : "playing";
        s += "\",\"you\":";
        s += pid;
        s += ",\"opp\":\"" + ha_json_escape(_p[opp].nick) + "\"";
        s += ",\"white\":";
        s += (chessSideOf(m, pid) == 0) ? "true" : "false";
        s += ",\"turn\":";
        s += turn;
        s += ",\"yourTurn\":";
        s += yourTurn ? "true" : "false";
        s += ",\"paused\":";
        s += paused ? "true" : "false";
        s += ",\"board\":\"" + chessBoardStr(m->core) + "\"";
        if(m->phase == 1) { // the mover's own legal moves; nobody else's are anyone's business
            s += ",\"moves\":[";
            if(yourTurn) {
                uint16_t mv[CH_MAX_MOVES];
                int n = chessGenLegal(m->core, mv);
                for(int i = 0; i < n; i++) {
                    if(i) s += ",";
                    s += (int)mv[i];
                }
            }
            s += "]";
        }
        s += ",\"check\":";
        s += chessInCheck(m->core) ? "true" : "false";
        s += ",\"last\":";
        s += (int)m->lastMove;
        s += ",\"remaining_ms\":";
        s += (unsigned long)rem;
        s += ",\"duration_ms\":";
        s += (unsigned long)CH_CLOCK_MS;
        s += ",\"other_remaining_ms\":";
        s += (unsigned long)m->clockMs[stm ^ 1];
        s += ",\"wtm\":";
        s += (stm == 0) ? "true" : "false";
        if(m->phase == 1) {
            s += ",\"claim3\":";
            s += (yourTurn && chessRepCount(m) >= 3) ? "true" : "false";
            s += ",\"claim50\":";
            s += (yourTurn && m->halfmove >= 100) ? "true" : "false";
        }
        s += ",\"offer\":";
        s += m->offerBy;
        if(m->phase == 2) {
            s += ",\"result\":\"";
            s += !m->winner ? "draw" : (m->winner == pid) ? "win" : "lose";
            s += "\",\"reason\":\"";
            s += chessReasonStr(m->reason);
            s += "\"";
        }
        s += "}";
        return s;
    }

#ifdef HA_CHESS_TEST
public:
    // Test-only: overwrite match slot 0's position after a normal challenge/accept, so
    // a scenario can set up a specific board without walking through the opening moves.
    // Requires slot 0 to already hold a live game (_matches.chess[0].used && phase == 1).
    void chessTestLoad(const char* board64, int stm, int rights, int ep, int halfmove,
                        uint32_t wms, uint32_t bms) {
        if(!_matches.chess[0].used || _matches.chess[0].phase != 1) return;
        if(!chessLoadCore(_matches.chess[0].core, board64, (uint8_t)stm, (uint8_t)rights, (int8_t)ep))
            return;
        _matches.chess[0].halfmove = (uint8_t)halfmove;
        _matches.chess[0].clockMs[0] = wms;
        _matches.chess[0].clockMs[1] = bms;
        _matches.chess[0].lastStamp = clockNow();
        _matches.chess[0].offerBy = 0;
        _matches.chess[0].lastMove = -1;
        _matches.chess[0].hist[0] = chessHash(_matches.chess[0].core);
        _matches.chess[0].histLen = 1;
        pushAll();
    }

    // Loads a scratch position (no match involved) and runs perft on it.
    static uint32_t chessTestPerft(const char* board64, int stm, int rights, int ep,
                                    int depth) {
        ChessCore c{};
        if(!chessLoadCore(c, board64, (uint8_t)stm, (uint8_t)rights, (int8_t)ep)) return 0;
        return chessPerft(c, depth);
    }
private:
#endif

    // ---------- spectrum (wavelength-style guessing) ----------
    // Which pack wins the pre-round vote; identical policy to wyrWinningPack().
    int spectrumWinningPack() {
        if(_spec.packCount == 0) return 0;
        int votes[TRIVIA_MAX_TOPICS] = {0};
        int total = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i) && _spec.vote[i] >= 0 && _spec.vote[i] < _spec.packCount) {
                votes[_spec.vote[i]]++;
                total++;
            }
        if(total == 0) return (int)random(_spec.packCount);
        int best = 0;
        for(int i = 1; i < _spec.packCount; i++)
            if(votes[i] > votes[best]) best = i;
        int tie[TRIVIA_MAX_TOPICS], tn = 0;
        for(int i = 0; i < _spec.packCount; i++)
            if(votes[i] == votes[best]) tie[tn++] = i;
        return tie[(int)random(tn)];
    }

    void spectrumClear() {
        partyClear(_spec.pt);
        _spec.pack = 0;
        _spec.card = 0;
        _spec.cardSeq = 0;
        _spec.psychic = 0;
        _spec.psychicSeq = 0;
        _spec.stage = 0;
        _spec.target = 0;
        _spec.clue[0] = '\0';
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _spec.vote[i] = -1;
            _spec.guess[i] = -1;
            _spec.gained[i] = 0;
        }
    }

    void spectrumReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_SPECTRUM) return;
        if(_spec.pt.phase != 0 && _spec.pt.phase != 4) return;
        if(_spec.pt.phase == 4 && val) spectrumClear(); // ready from final -> new game
        _spec.pt.ready[pid] = val;
        spectrumCheckStart();
        pushAll();
    }

    void spectrumVote(uint8_t pid, int pack) {
        if(_active != HA_GAME_SPECTRUM || _spec.pt.phase != 0) return;
        if(pack < 0 || pack >= _spec.packCount) return;
        _spec.vote[pid] = (int8_t)pack;
        pushAll();
    }

    void spectrumCheckStart() {
        if(_spec.packCount == 0) return;
        Party& pt = _spec.pt;
        if(pt.phase == 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    // The psychic rotates across rounds: the (psychicSeq mod N)-th connected player.
    uint8_t spectrumPickPsychic() {
        int n = connectedCount();
        if(n <= 0) return 0;
        int want = _spec.psychicSeq % n;
        int seen = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            if(seen == want) return i;
            seen++;
        }
        return 0;
    }

    void spectrumNextRound(uint32_t now) {
        Party& pt = _spec.pt;
        WyrPack& pk = _spec.packs[_spec.pack];
        if(pt.round >= SPECTRUM_ROUNDS || pk.count == 0) {
            pt.phase = 4; // final
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        _spec.psychic = spectrumPickPsychic();
        _spec.psychicSeq++;
        _spec.card = (uint8_t)(_spec.cardSeq % pk.count);
        _spec.cardSeq++;
        _spec.target = 5 + (int)random(91); // 5..95, avoid the very edges
        _spec.stage = 0; // clue first
        _spec.clue[0] = '\0';
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _spec.guess[i] = -1;
            _spec.gained[i] = 0;
        }
        pt.deadline = now + (uint32_t)SPECTRUM_CLUE_SECS * 1000;
        pt.phase = 2;
        pushAll();
    }

    void spectrumClue(uint8_t pid, const char* text) {
        if(_active != HA_GAME_SPECTRUM || _spec.pt.phase != 2 || _spec.stage != 0) return;
        if(pid != _spec.psychic) return;
        strlcpy(_spec.clue, text, sizeof(_spec.clue));
        _spec.stage = 1; // move to guessing
        _spec.pt.deadline = clockNow() + (uint32_t)SPECTRUM_GUESS_SECS * 1000;
        hostEvent(HA_HOST_EVT_ROLE, pid, 0, 0, _spec.clue);
        pushAll();
    }

    bool spectrumAllGuessed() {
        int guessers = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i) || i == _spec.psychic) continue;
            guessers++;
            if(_spec.guess[i] < 0) return false;
        }
        return guessers >= 1;
    }

    void spectrumGuess(uint8_t pid, int val) {
        if(_active != HA_GAME_SPECTRUM || _spec.pt.phase != 2 || _spec.stage != 1) return;
        if(pid == _spec.psychic) return; // the clue-giver doesn't guess
        if(val < 0 || val > 100) return;
        _spec.guess[pid] = (int8_t)val;
        if(spectrumAllGuessed()) spectrumReveal(clockNow());
        else pushAll();
    }

    // Points by closeness of the guess (0..100) to the hidden target (0..100).
    // A tight bullseye (±2) for landing right on it, then two 5-wide rings,
    // matching the dial's three scoring wedges exactly.
    static int spectrumPoints(int target, int guess) {
        int d = target - guess;
        if(d < 0) d = -d;
        if(d <= 2) return 4;
        if(d <= 7) return 3;
        if(d <= 12) return 2;
        return 0;
    }

    void spectrumReveal(uint32_t now) {
        int sum = 0, guessers = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!_p[i].used || i == _spec.psychic || _spec.guess[i] < 0) continue;
            int pts = spectrumPoints(_spec.target, _spec.guess[i]);
            _spec.gained[i] = pts;
            awardScore(i, pts, "spectrum");
            sum += pts;
            guessers++;
        }
        // The psychic scores by how well the group did: the average guesser score,
        // so a clue that lands everyone near the target is worth the most.
        if(_spec.psychic && guessers > 0) {
            int avg = (sum + guessers / 2) / guessers;
            _spec.gained[_spec.psychic] = avg;
            awardScore(_spec.psychic, avg, "clue");
        }
        hostEvent(HA_HOST_EVT_ROUND_COMPLETE, _spec.psychic, 0, _spec.pt.round);
        _spec.pt.phase = 3;
        _spec.pt.revealUntil = now + SPECTRUM_REVEAL_MS;
        pushAll();
    }

    void spectrumAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_SPECTRUM || _spec.pt.phase != 4) return;
        spectrumClear();
        pushAll();
    }

    void spectrumTick(uint32_t now) {
        Party& pt = _spec.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                resetScoresAll();
                _spec.pack = (uint8_t)spectrumWinningPack();
                _spec.psychicSeq = 0;
                _spec.cardSeq = 0;
                spectrumNextRound(now);
            }
        } else if(pt.phase == 2) {
            if(_spec.stage == 0) {
                // Clue window expired with no clue: move on to guessing anyway so a
                // silent/absent psychic can't stall the game.
                if(haTimeReached(now, pt.deadline)) {
                    _spec.stage = 1;
                    pt.deadline = now + (uint32_t)SPECTRUM_GUESS_SECS * 1000;
                    pushAll();
                }
            } else {
                if(haTimeReached(now, pt.deadline) || spectrumAllGuessed())
                    spectrumReveal(now);
            }
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) spectrumNextRound(now);
        }
    }

    String spectrumJson(uint8_t pid) {
        Party& pt = _spec.pt;
        if(pt.phase == 0) {
            String s = String("{\"t\":\"spectrum\",\"phase\":\"lobby\",\"you\":") + pid +
                       ",\"players\":" + partyPlayersJson(pt);
            s += ",\"packs\":[";
            int votes[TRIVIA_MAX_TOPICS] = {0};
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _spec.vote[i] >= 0 && _spec.vote[i] < _spec.packCount)
                    votes[_spec.vote[i]]++;
            for(int i = 0; i < _spec.packCount; i++) {
                if(i) s += ",";
                s += "{\"name\":\"" + ha_json_escape(_spec.packs[i].name.c_str()) +
                     "\",\"votes\":" + votes[i] + "}";
            }
            s += "],\"myvote\":" + String((int)_spec.vote[pid]) + "}";
            return s;
        }
        if(pt.phase == 1)
            return String("{\"t\":\"spectrum\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"spectrum\",\"phase\":\"final\",\"board\":") + triviaBoard() +
                   "}";

        WyrPack& pk = _spec.packs[_spec.pack];
        const char* left = pk.items[_spec.card].a.c_str();
        const char* right = pk.items[_spec.card].b.c_str();
        bool mePsychic = (pid == _spec.psychic);
        bool reveal = (pt.phase == 3);
        const char* stage = reveal ? "reveal" : (_spec.stage == 0 ? "clue" : "guess");

        String s = String("{\"t\":\"spectrum\",\"phase\":\"play\",\"stage\":\"") + stage +
                   "\",\"round\":" + pt.round + ",\"rounds\":" + SPECTRUM_ROUNDS + ",\"left\":\"" +
                   ha_json_escape(left) + "\",\"right\":\"" + ha_json_escape(right) +
                   "\",\"psychic\":\"" + ha_json_escape(_p[_spec.psychic].nick) +
                   "\",\"iam\":" + (mePsychic ? "true" : "false") + ",\"paused\":" +
                   (_criticalPausePid ? "true" : "false");
        // The psychic sees the target during the clue stage; on reveal everyone does.
        if(reveal || mePsychic) {
            s += ",\"target\":";
            s += _spec.target;
        }
        if(_spec.stage == 1 || reveal) {
            s += ",\"clue\":\"";
            s += ha_json_escape(_spec.clue);
            s += "\"";
        }
        if(!mePsychic) {
            s += ",\"myguess\":";
            s += (int)_spec.guess[pid];
        }
        if(reveal) {
            s += ",\"guesses\":[";
            bool first = true;
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
                if(!_p[i].used || i == _spec.psychic || _spec.guess[i] < 0) continue;
                if(!first) s += ",";
                first = false;
                s += "{\"nick\":\"" + ha_json_escape(_p[i].nick) + "\",\"g\":" +
                     (int)_spec.guess[i] + ",\"pts\":" + _spec.gained[i] + "}";
            }
            s += "]";
            s += ",\"mygain\":";
            s += _spec.gained[pid];
            appendTimer(s, pt.revealUntil, SPECTRUM_REVEAL_MS);
        } else {
            appendTimer(
                s,
                pt.deadline,
                (uint32_t)(_spec.stage == 0 ? SPECTRUM_CLUE_SECS : SPECTRUM_GUESS_SECS) * 1000);
        }
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }

    // ---------- Kiss Marry Kill (predict a player's picks) ----------
    int kmkWinningPack() {
        if(_kmk.packCount == 0) return 0;
        int votes[TRIVIA_MAX_TOPICS] = {0};
        int total = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(playerOnline(i) && _kmk.vote[i] >= 0 && _kmk.vote[i] < _kmk.packCount) {
                votes[_kmk.vote[i]]++;
                total++;
            }
        if(total == 0) return (int)random(_kmk.packCount);
        int best = 0;
        for(int i = 1; i < _kmk.packCount; i++)
            if(votes[i] > votes[best]) best = i;
        int tie[TRIVIA_MAX_TOPICS], tn = 0;
        for(int i = 0; i < _kmk.packCount; i++)
            if(votes[i] == votes[best]) tie[tn++] = i;
        return tie[(int)random(tn)];
    }

    void kmkClear() {
        partyClear(_kmk.pt);
        _kmk.pack = 0;
        _kmk.nameSeq = 0;
        _kmk.chooser = 0;
        _kmk.chooserSeq = 0;
        _kmk.stage = 0;
        for(int i = 0; i < 3; i++) {
            _kmk.person[i] = 0;
            _kmk.cLabel[i] = -1;
        }
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _kmk.vote[i] = -1;
            _kmk.guessed[i] = false;
            _kmk.gained[i] = 0;
            for(int j = 0; j < 3; j++) _kmk.gLabel[i][j] = -1;
        }
    }

    void kmkReady(uint8_t pid, bool val) {
        if(_active != HA_GAME_KMK) return;
        if(_kmk.pt.phase != 0 && _kmk.pt.phase != 4) return;
        if(_kmk.pt.phase == 4 && val) kmkClear();
        _kmk.pt.ready[pid] = val;
        kmkCheckStart();
        pushAll();
    }

    void kmkVote(uint8_t pid, int pack) {
        if(_active != HA_GAME_KMK || _kmk.pt.phase != 0) return;
        if(pack < 0 || pack >= _kmk.packCount) return;
        _kmk.vote[pid] = (int8_t)pack;
        pushAll();
    }

    void kmkCheckStart() {
        if(_kmk.packCount == 0) return;
        Party& pt = _kmk.pt;
        if(pt.phase == 0 && partyAllReady(pt)) {
            pt.phase = 1;
            pt.countdownEnd = clockNow() + (uint32_t)PARTY_COUNTDOWN * 1000;
            pt.lastSec = -1;
        } else if(pt.phase == 1 && !partyAllReady(pt)) {
            pt.phase = 0;
        }
    }

    // The chooser rotates: the (chooserSeq mod N)-th connected player.
    uint8_t kmkPickChooser() {
        int n = connectedCount();
        if(n <= 0) return 0;
        int want = _kmk.chooserSeq % n, seen = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i)) continue;
            if(seen == want) return i;
            seen++;
        }
        return 0;
    }

    void kmkNextRound(uint32_t now) {
        Party& pt = _kmk.pt;
        WordPack& pk = _kmk.packs[_kmk.pack];
        if(pt.round >= KMK_ROUNDS || pk.count < 3) {
            pt.phase = 4; // final (need at least three names to play)
            hostEvent(HA_HOST_EVT_GAME_FINAL);
            pushAll();
            return;
        }
        pt.round++;
        _kmk.chooser = kmkPickChooser();
        _kmk.chooserSeq++;
        // three distinct people, walking the pack from a rotating offset
        uint8_t base = (uint8_t)(_kmk.nameSeq % pk.count);
        _kmk.nameSeq += 3;
        _kmk.person[0] = base;
        _kmk.person[1] = (uint8_t)((base + 1 + random(pk.count - 2)) % pk.count);
        do {
            _kmk.person[2] = (uint8_t)(random(pk.count));
        } while(_kmk.person[2] == _kmk.person[0] || _kmk.person[2] == _kmk.person[1]);
        _kmk.stage = 0; // chooser assigns first
        for(int i = 0; i < 3; i++) _kmk.cLabel[i] = -1;
        for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
            _kmk.guessed[i] = false;
            _kmk.gained[i] = 0;
            for(int j = 0; j < 3; j++) _kmk.gLabel[i][j] = -1;
        }
        pt.deadline = now + (uint32_t)KMK_CHOOSE_SECS * 1000;
        pt.phase = 2;
        pushAll();
    }

    // kiss/marry/kill are person indices 0..2; build a per-person label array
    // (0 kiss, 1 marry, 2 kill). Returns false unless it's a valid permutation.
    static bool kmkToLabels(int kiss, int marry, int kill, int8_t out[3]) {
        int a[3] = {kiss, marry, kill};
        for(int i = 0; i < 3; i++)
            if(a[i] < 0 || a[i] > 2) return false;
        if(kiss == marry || kiss == kill || marry == kill) return false;
        out[kiss] = 0;
        out[marry] = 1;
        out[kill] = 2;
        return true;
    }

    bool kmkAllGuessed() {
        int guessers = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!playerOnline(i) || i == _kmk.chooser) continue;
            guessers++;
            if(!_kmk.guessed[i]) return false;
        }
        return guessers >= 1;
    }

    void kmkAssign(uint8_t pid, int kiss, int marry, int kill) {
        if(_active != HA_GAME_KMK || _kmk.pt.phase != 2) return;
        int8_t labels[3];
        if(!kmkToLabels(kiss, marry, kill, labels)) return;
        if(_kmk.stage == 0) {
            if(pid != _kmk.chooser) return; // only the chooser sets the secret
            for(int i = 0; i < 3; i++) _kmk.cLabel[i] = labels[i];
            _kmk.stage = 1;
            _kmk.pt.deadline = clockNow() + (uint32_t)KMK_GUESS_SECS * 1000;
            hostEvent(HA_HOST_EVT_ROLE, pid, 0, 0, "chooser");
            pushAll();
        } else {
            if(pid == _kmk.chooser) return; // the chooser doesn't guess
            for(int i = 0; i < 3; i++) _kmk.gLabel[pid][i] = labels[i];
            _kmk.guessed[pid] = true;
            if(kmkAllGuessed()) kmkReveal(clockNow());
            else pushAll();
        }
    }

    void kmkReveal(uint32_t now) {
        int sum = 0, guessers = 0;
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
            if(!_p[i].used || i == _kmk.chooser || !_kmk.guessed[i]) continue;
            int hit = 0;
            for(int j = 0; j < 3; j++)
                if(_kmk.gLabel[i][j] == _kmk.cLabel[j]) hit++;
            _kmk.gained[i] = hit; // 0, 1 or 3 (two right forces the third)
            awardScore(i, hit, "kmk");
            sum += hit;
            guessers++;
        }
        if(_kmk.chooser && guessers > 0) {
            int avg = (sum + guessers / 2) / guessers;
            _kmk.gained[_kmk.chooser] = avg;
            awardScore(_kmk.chooser, avg, "kmk");
        }
        hostEvent(HA_HOST_EVT_ROUND_COMPLETE, _kmk.chooser, 0, _kmk.pt.round);
        _kmk.pt.phase = 3;
        _kmk.pt.revealUntil = now + KMK_REVEAL_MS;
        pushAll();
    }

    void kmkAgain(uint8_t pid) {
        (void)pid;
        if(_active != HA_GAME_KMK || _kmk.pt.phase != 4) return;
        kmkClear();
        pushAll();
    }

    void kmkTick(uint32_t now) {
        Party& pt = _kmk.pt;
        if(pt.phase == 1) {
            if(partyCountdownDone(pt, now)) {
                pt.round = 0;
                resetScoresAll();
                _kmk.pack = (uint8_t)kmkWinningPack();
                _kmk.chooserSeq = 0;
                _kmk.nameSeq = 0;
                kmkNextRound(now);
            }
        } else if(pt.phase == 2) {
            if(_kmk.stage == 0) {
                if(haTimeReached(now, pt.deadline)) { // chooser stalled: pick for them
                    _kmk.cLabel[0] = 0;
                    _kmk.cLabel[1] = 1;
                    _kmk.cLabel[2] = 2;
                    _kmk.stage = 1;
                    pt.deadline = now + (uint32_t)KMK_GUESS_SECS * 1000;
                    pushAll();
                }
            } else {
                if(haTimeReached(now, pt.deadline) || kmkAllGuessed()) kmkReveal(now);
            }
        } else if(pt.phase == 3) {
            if(haTimeReached(now, pt.revealUntil)) kmkNextRound(now);
        }
    }

    // Emit a player's K/M/K labels for the three people as an array of 0/1/2/-1.
    String kmkLabelsJson(const int8_t* lab) {
        String s = "[";
        for(int i = 0; i < 3; i++) {
            if(i) s += ",";
            s += (int)lab[i];
        }
        s += "]";
        return s;
    }

    String kmkJson(uint8_t pid) {
        Party& pt = _kmk.pt;
        if(pt.phase == 0) {
            String s = String("{\"t\":\"kmk\",\"phase\":\"lobby\",\"you\":") + pid +
                       ",\"players\":" + partyPlayersJson(pt);
            s += ",\"packs\":[";
            int votes[TRIVIA_MAX_TOPICS] = {0};
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
                if(playerOnline(i) && _kmk.vote[i] >= 0 && _kmk.vote[i] < _kmk.packCount)
                    votes[_kmk.vote[i]]++;
            for(int i = 0; i < _kmk.packCount; i++) {
                if(i) s += ",";
                s += "{\"name\":\"" + ha_json_escape(_kmk.packs[i].name.c_str()) +
                     "\",\"votes\":" + votes[i] + "}";
            }
            s += "],\"myvote\":" + String((int)_kmk.vote[pid]) + "}";
            return s;
        }
        if(pt.phase == 1)
            return String("{\"t\":\"kmk\",\"phase\":\"countdown\",\"sec\":") +
                   partyCountdownSec(pt) + "}";
        if(pt.phase == 4)
            return String("{\"t\":\"kmk\",\"phase\":\"final\",\"board\":") + triviaBoard() + "}";

        WordPack& pk = _kmk.packs[_kmk.pack];
        bool me = (pid == _kmk.chooser);
        bool reveal = (pt.phase == 3);
        const char* stage = reveal ? "reveal" : (_kmk.stage == 0 ? "choose" : "guess");

        String s = String("{\"t\":\"kmk\",\"phase\":\"play\",\"stage\":\"") + stage +
                   "\",\"round\":" + pt.round + ",\"rounds\":" + KMK_ROUNDS + ",\"chooser\":\"" +
                   ha_json_escape(_p[_kmk.chooser].nick) + "\",\"iam\":" + (me ? "true" : "false") +
                   ",\"paused\":" + (_criticalPausePid ? "true" : "false") + ",\"people\":[";
        for(int i = 0; i < 3; i++) {
            if(i) s += ",";
            s += "\"" + ha_json_escape(pk.words[_kmk.person[i]].c_str()) + "\"";
        }
        s += "]";
        // The chooser sees their own picks during the guess stage; on reveal everyone
        // sees the chooser's actual assignment.
        if(reveal || (me && _kmk.stage == 1))
            s += ",\"answer\":" + kmkLabelsJson(_kmk.cLabel);
        if(!me) s += ",\"mine\":" + kmkLabelsJson(_kmk.gLabel[pid]);
        if(reveal) {
            s += ",\"guesses\":[";
            bool first = true;
            for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++) {
                if(!_p[i].used || i == _kmk.chooser || !_kmk.guessed[i]) continue;
                if(!first) s += ",";
                first = false;
                s += "{\"nick\":\"" + ha_json_escape(_p[i].nick) + "\",\"pick\":" +
                     kmkLabelsJson(_kmk.gLabel[i]) + ",\"pts\":" + _kmk.gained[i] + "}";
            }
            s += "],\"mygain\":" + String(_kmk.gained[pid]);
            appendTimer(s, pt.revealUntil, KMK_REVEAL_MS);
        } else {
            appendTimer(
                s,
                pt.deadline,
                (uint32_t)(_kmk.stage == 0 ? KMK_CHOOSE_SECS : KMK_GUESS_SECS) * 1000);
        }
        s += ",\"scores\":" + playersJson() + "}";
        return s;
    }
};
