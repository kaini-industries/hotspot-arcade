#include "ha_session.h"
#include "ha_storage.h"
#include "../hotspot_arcade_i.h"

// Everything here runs on the GUI thread (RX drained from the global custom event
// handler), so app state is single-threaded: no locking needed.

enum {
    RXS_SYNC,
    RXS_TYPE,
    RXS_LEN0,
    RXS_LEN1,
    RXS_PAYLOAD,
    RXS_CRC
};

// ---------------- feedback (respects settings) ----------------

// A light blip for minor events (a player joins).
static void feedback_blip(HotspotArcadeApp* app) {
    if(app->vibro_on) notification_message(app->notifications, &sequence_single_vibro);
}

// A success cue for a scored moment (trivia reveal, a Connect Four win).
static void feedback_success(HotspotArcadeApp* app) {
    if(app->vibro_on) notification_message(app->notifications, &sequence_single_vibro);
    if(app->sound_on) notification_message(app->notifications, &sequence_success);
}

// ---------------- console / event feed ----------------

static void console_add(HotspotArcadeApp* app, const char* line) {
    furi_string_cat_str(app->console, line);
    furi_string_cat_str(app->console, "\n");
    size_t sz = furi_string_size(app->console);
    if(sz > HA_CONSOLE_MAX) furi_string_right(app->console, sz - HA_CONSOLE_MAX / 2);
}

static const char* host_game_name(uint8_t game) {
    switch(game) {
    case HA_GAME_TRIVIA: return "Trivia";
    case HA_GAME_CONNECT4: return "Connect 4";
    case HA_GAME_TICTACTOE: return "Tic-Tac-Toe";
    case HA_GAME_DOTS: return "Dots";
    case HA_GAME_DRAW: return "Draw";
    case HA_GAME_PONG: return "Pong";
    case HA_GAME_REACT: return "React";
    case HA_GAME_WYR: return "Would You Rather";
    case HA_GAME_SCRAMBLE: return "Scramble";
    case HA_GAME_REVERSI: return "Reversi";
    case HA_GAME_GUESSCOLOR: return "Guess Color";
    case HA_GAME_BATTLESHIP: return "Battleship";
    case HA_GAME_SPECTRUM: return "Spectrum";
    case HA_GAME_KMK: return "KMK";
    case HA_GAME_CHESS: return "Chess";
    default: return "Arcade";
    }
}

// ---------------- roster ----------------

int ha_player_count(HotspotArcadeApp* app) {
    int n = 0;
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used && app->players[i].online) n++;
    return n;
}

static int player_find(HotspotArcadeApp* app, uint8_t pid) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used && app->players[i].online && app->players[i].pid == pid) return i;
    return -1;
}

static int player_find_identity(HotspotArcadeApp* app, const char* identity) {
    if(!identity || !identity[0]) return -1;
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used && strcmp(app->players[i].identity, identity) == 0) return i;
    return -1;
}

static const char* player_nick(HotspotArcadeApp* app, uint8_t pid) {
    int idx = player_find(app, pid);
    return idx >= 0 ? app->players[idx].nick : "PLAYER";
}

static bool format_host_event(
    HotspotArcadeApp* app,
    const uint8_t* p,
    uint16_t len,
    char* out,
    size_t out_size,
    bool* success) {
    if(success) *success = false;
    if(!p || len < 7 || p[0] != HA_HOST_EVENT_VERSION || !out || out_size == 0) return false;
    uint8_t kind = p[1], game = p[2], actor = p[3], target = p[4];
    int16_t value = (int16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
    char text[HA_HOST_EVENT_TEXT_MAX + 1];
    size_t text_len = len - 7;
    if(text_len > HA_HOST_EVENT_TEXT_MAX) return false;
    if(text_len) memcpy(text, p + 7, text_len);
    text[text_len] = '\0';
    const char* gn = host_game_name(game);
    const char* a = player_nick(app, actor);
    const char* b = player_nick(app, target);
    switch(kind) {
    case HA_HOST_EVT_MATCH_STARTED:
        snprintf(out, out_size, "%s: %s vs %s", gn, a, b);
        break;
    case HA_HOST_EVT_CHAT:
        snprintf(out, out_size, "%s: %s", a, text);
        break;
    case HA_HOST_EVT_ROLE:
        snprintf(out, out_size, "%s: %s %s", gn, a, text);
        break;
    case HA_HOST_EVT_ROUND_WIN:
        snprintf(out, out_size, "%s: %s beat %s%s%s", gn, a, b,
                 text[0] ? " (" : "", text[0] ? text : "");
        if(text[0] && strlen(out) + 1 < out_size) strlcat(out, ")", out_size);
        if(success) *success = true;
        break;
    case HA_HOST_EVT_ROUND_DRAW:
        snprintf(out, out_size, "%s: %s / %s draw%s%s", gn, a, b,
                 text[0] ? " (" : "", text[0] ? text : "");
        if(text[0] && strlen(out) + 1 < out_size) strlcat(out, ")", out_size);
        if(success) *success = true;
        break;
    case HA_HOST_EVT_ROUND_COMPLETE:
        snprintf(out, out_size, "%s: round %d complete", gn, (int)value);
        if(success) *success = true;
        break;
    case HA_HOST_EVT_GAME_FINAL:
        snprintf(out, out_size, "%s: game complete", gn);
        if(success) *success = true;
        break;
    default:
        return false;
    }
    return true;
}

// Nicknames are shown uppercase everywhere. The ESP already uppercases on hello,
// but a board running older firmware doesn't, and the roster is on every screen
// here. ASCII only, so UTF-8 nicknames pass through untouched.
static void nick_upper(char* s) {
    for(; s && *s; s++)
        if(*s >= 'a' && *s <= 'z') *s -= 32;
}

static void player_join(
    HotspotArcadeApp* app,
    uint8_t pid,
    const char* identity,
    const char* nick) {
    int idx = player_find_identity(app, identity);
    bool fresh = idx < 0;
    if(idx < 0) {
        int stalePid = player_find(app, pid);
        if(stalePid >= 0) {
            app->players[stalePid].online = false;
            app->players[stalePid].pid = 0;
        }
        for(int i = 0; i < HA_MAX_PLAYERS; i++) {
            if(!app->players[i].used) {
                idx = i;
                break;
            }
        }
    }
    if(idx < 0) return;
    HaPlayer* p = &app->players[idx];
    p->used = true;
    p->online = true;
    p->pid = pid;
    strlcpy(p->identity, identity ? identity : "", sizeof(p->identity));
    strlcpy(p->nick, (nick && nick[0]) ? nick : "PLAYER", HA_NICK_LEN);
    nick_upper(p->nick);
    // A resumed socket and an in-game identity edit both reannounce JOIN for
    // the same pid. Keep the host scoreboard mirror; only a genuinely new seat
    // starts at zero (an expired seat emits LEAVE before its pid can be reused).
    if(fresh) p->score = 0;
}

static void player_leave(HotspotArcadeApp* app, uint8_t pid) {
    int idx = player_find(app, pid);
    if(idx >= 0) {
        app->players[idx].online = false;
        app->players[idx].pid = 0;
    }
}

static void player_score(HotspotArcadeApp* app, uint8_t pid, int delta) {
    int idx = player_find(app, pid);
    if(idx >= 0) {
        int64_t next = (int64_t)app->players[idx].score + delta;
        if(next > INT32_MAX) next = INT32_MAX;
        if(next < INT32_MIN) next = INT32_MIN;
        app->players[idx].score = (int32_t)next;
    }
}

static void roster_clear(HotspotArcadeApp* app) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++) {
        app->players[i].used = false;
        app->players[i].online = false;
        app->players[i].pid = 0;
    }
}

static void roster_detach_all(HotspotArcadeApp* app) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++) {
        app->players[i].online = false;
        app->players[i].pid = 0;
    }
}

// ---------------- trivia pack streaming ----------------
// The Flipper no longer hosts trivia; it just streams every pack on the SD card to
// the ESP as votable topics at session start, and the ESP orchestrates the game.

static void copy_trim(const char* start, const char* end, FuriString* out) {
    while(start < end && (*start == ' ' || *start == '\t'))
        start++;
    while(end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    furi_string_set(out, "");
    for(const char* p = start; p < end; p++)
        furi_string_push_back(out, *p);
}

static void json_escape_cat(FuriString* out, const char* s) {
    for(const char* p = s; *p; p++) {
        char c = *p;
        if(c == '"' || c == '\\') {
            furi_string_push_back(out, '\\');
            furi_string_push_back(out, c);
        } else if(c == '\n') {
            furi_string_cat_str(out, "\\n");
        } else if((unsigned char)c >= 0x20) {
            furi_string_push_back(out, c);
        }
    }
}

// Stream one pack file as generic blocks. The grammar is the whole contract:
// "Key: value" lines; a line of "---", or a blank line, ends a block. A "Pack:" key
// names the pack and is not part of an item. Everything else is shipped verbatim as
// a JSON object of the file's own (lowercased) keys — this app deliberately does not
// know what a question, prompt or word is. The ESP owns all of that.
static void content_send_item(
    HotspotArcadeApp* app,
    FuriString* obj,
    bool* any,
    uint16_t* item_count) {
    if(!*any) return; // nothing accumulated
    furi_string_cat_str(obj, "}");
    ha_proto_send(
        app->uart,
        HA_MSG_CONTENT_ITEM,
        (const uint8_t*)furi_string_get_cstr(obj),
        furi_string_size(obj));
    (*item_count)++;
    furi_string_set(obj, "{");
    *any = false;
}

static void content_stream_pack(
    HotspotArcadeApp* app,
    uint8_t game,
    const char* content,
    const char* fallback,
    uint16_t* pack_count,
    uint16_t* item_count) {
    // Pass one: find the pack name so CONTENT_PACK can go first.
    FuriString* name = furi_string_alloc_set_str(fallback);
    for(const char* p = content; p && *p;) {
        const char* eol = strchr(p, '\n');
        if(!eol) eol = p + strlen(p);
        if(strncmp(p, "Pack:", 5) == 0) {
            FuriString* v = furi_string_alloc();
            copy_trim(p + 5, eol, v);
            if(furi_string_size(v)) furi_string_set(name, v);
            furi_string_free(v);
            break;
        }
        p = (*eol) ? eol + 1 : eol;
    }

    uint8_t hdr[1 + 64];
    hdr[0] = game;
    size_t nl = furi_string_size(name);
    if(nl > sizeof(hdr) - 1) nl = sizeof(hdr) - 1;
    memcpy(hdr + 1, furi_string_get_cstr(name), nl);
    ha_proto_send(app->uart, HA_MSG_CONTENT_PACK, hdr, 1 + nl);
    (*pack_count)++;
    furi_string_free(name);

    // Pass two: blocks.
    FuriString* obj = furi_string_alloc_set_str("{");
    FuriString* key = furi_string_alloc();
    FuriString* val = furi_string_alloc();
    bool any = false;
    for(const char* p = content; p && *p;) {
        const char* eol = strchr(p, '\n');
        if(!eol) eol = p + strlen(p);

        // Trim the line to decide whether it is a separator.
        const char* s = p;
        const char* e = eol;
        while(s < e && (*s == ' ' || *s == '\t' || *s == '\r'))
            s++;
        while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            e--;

        bool sep = (s == e) || (e - s == 3 && strncmp(s, "---", 3) == 0);
        if(sep) {
            content_send_item(app, obj, &any, item_count);
        } else {
            const char* colon = memchr(s, ':', (size_t)(e - s));
            if(colon) {
                copy_trim(s, colon, key);
                copy_trim(colon + 1, e, val);
                // Keys are case-insensitive on the wire; "Pack:" is metadata, already
                // consumed above, so it never becomes part of an item.
                char* k = (char*)furi_string_get_cstr(key);
                for(char* c = k; *c; c++)
                    if(*c >= 'A' && *c <= 'Z') *c += 32;
                if(furi_string_size(key) && strcmp(k, "pack") != 0) {
                    if(any) furi_string_cat_str(obj, ",");
                    furi_string_cat_str(obj, "\"");
                    json_escape_cat(obj, k);
                    furi_string_cat_str(obj, "\":\"");
                    json_escape_cat(obj, furi_string_get_cstr(val));
                    furi_string_cat_str(obj, "\"");
                    any = true;
                }
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    content_send_item(app, obj, &any, item_count);
    furi_string_free(obj);
    furi_string_free(key);
    furi_string_free(val);
}

#define HA_MAX_TOPICS (6)

// Stream every .txt pack in one dir as votable topics, skipping names already streamed.
// `seen` holds the filenames taken so far; *topics is the running total across dirs.
static void ha_content_stream_dir(
    HotspotArcadeApp* app,
    Storage* storage,
    const char* dir_path,
    uint8_t game,
    char seen[HA_MAX_TOPICS][80],
    int* topics,
    uint16_t* pack_count,
    uint16_t* item_count) {
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, dir_path)) {
        FileInfo info;
        char name[80];
        while(*topics < HA_MAX_TOPICS && storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            size_t nl = strlen(name);
            const char* e = name + (nl >= 4 ? nl - 4 : 0);
            if(nl < 5 || e[0] != '.' || (e[1] | 32) != 't' || (e[2] | 32) != 'x' ||
               (e[3] | 32) != 't')
                continue;
            bool dup = false;
            for(int i = 0; i < *topics && !dup; i++)
                dup = (strcmp(seen[i], name) == 0);
            if(dup) continue; // same filename in apps_data already won
            FuriString* path = furi_string_alloc();
            furi_string_printf(path, "%s/%s", dir_path, name);
            FuriString* content = furi_string_alloc();
            if(ha_storage_read_file(furi_string_get_cstr(path), content, 16384)) {
                FuriString* fb = furi_string_alloc_set_str(name);
                furi_string_left(fb, nl - 4); // drop ".txt"
                content_stream_pack(
                    app, game, furi_string_get_cstr(content), furi_string_get_cstr(fb),
                    pack_count, item_count);
                furi_string_free(fb);
                strlcpy(seen[*topics], name, sizeof(seen[0]));
                (*topics)++;
            }
            furi_string_free(content);
            furi_string_free(path);
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
}

// Stream one game's packs from packs/<sub> (user before bundled), into its per-game cap.
// `sub` is the game dir, optionally with a "/<lang>" suffix for a translated set.
static void ha_stream_subdir(
    HotspotArcadeApp* app,
    Storage* storage,
    uint8_t game,
    const char* sub,
    char seen[HA_MAX_TOPICS][80],
    int* topics,
    uint16_t* pack_count,
    uint16_t* item_count) {
    FuriString* d = furi_string_alloc();
    furi_string_printf(d, "%s/%s", HA_USER_PACKS_DIR, sub);
    ha_content_stream_dir(
        app, storage, furi_string_get_cstr(d), game, seen, topics, pack_count, item_count);
    furi_string_printf(d, "%s/%s", HA_BUNDLED_PACKS_DIR, sub);
    ha_content_stream_dir(
        app, storage, furi_string_get_cstr(d), game, seen, topics, pack_count, item_count);
    furi_string_free(d);
}

// Stream every content game's packs to the ESP. User packs win over bundled (name clash
// and the topic cap). If a language is selected, each game prefers packs/<game>/<lang>/
// and falls back to English (packs/<game>/) when that language has none, so a partly
// translated language still plays. English content is the unchanged root.
static void send_config(HotspotArcadeApp* app);

static void ha_content_stream_packs(HotspotArcadeApp* app) {
    ha_proto_send(app->uart, HA_MSG_CONTENT_CLEAR, NULL, 0);
    furi_delay_ms(2);
    // CONFIG is staged by protocol-v18 firmware while a content transaction is
    // open, so locale and packs become visible together at CONTENT_COMMIT.
    send_config(app);
    uint16_t pack_count = 0, item_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    // `seen`/`topics` de-duplicate and cap packs PER GAME (each game stores its own set
    // on the ESP), so every game restarts `topics` at 0. Only the dir and game byte differ.
    char seen[HA_MAX_TOPICS][80];
    const char* lang = app->lang; // "" = English

    static const struct {
        uint8_t game;
        const char* sub;
    } games[] = {
        {HA_GAME_TRIVIA, "trivia"},
        {HA_GAME_WYR, "wyr"},
        {HA_GAME_SCRAMBLE, "scramble"},
        {HA_GAME_DRAW, "draw"},
        {HA_GAME_SPECTRUM, "spectrum"},
        {HA_GAME_KMK, "kmk"},
    };
    for(unsigned g = 0; g < sizeof(games) / sizeof(games[0]); g++) {
        int topics = 0;
        // Prefer the selected language's packs...
        if(lang[0]) {
            FuriString* s = furi_string_alloc();
            furi_string_printf(s, "%s/%s", games[g].sub, lang);
            ha_stream_subdir(
                app, storage, games[g].game, furi_string_get_cstr(s), seen, &topics,
                &pack_count, &item_count);
            furi_string_free(s);
        }
        // ...falling back to the English root when the language has none for this game.
        if(topics == 0)
            ha_stream_subdir(
                app, storage, games[g].game, games[g].sub, seen, &topics,
                &pack_count, &item_count);
        // Trivia also sweeps the legacy trivia-only dirs (English, pre-packs/ layout),
        // but only when it's on the English set.
        if(games[g].game == HA_GAME_TRIVIA && (!lang[0] || topics == 0)) {
            ha_content_stream_dir(
                app, storage, HA_USER_TRIVIA_DIR, HA_GAME_TRIVIA, seen, &topics,
                &pack_count, &item_count);
            ha_content_stream_dir(
                app, storage, HA_BUNDLED_TRIVIA_DIR, HA_GAME_TRIVIA, seen, &topics,
                &pack_count, &item_count);
        }
    }

    furi_record_close(RECORD_STORAGE);
    uint8_t commit[4] = {
        (uint8_t)(pack_count & 0xFF),
        (uint8_t)(pack_count >> 8),
        (uint8_t)(item_count & 0xFF),
        (uint8_t)(item_count >> 8),
    };
    ha_proto_send(app->uart, HA_MSG_CONTENT_COMMIT, commit, sizeof(commit));
}

// ---------------- game selection ----------------

void ha_select_game(HotspotArcadeApp* app, uint8_t game) {
    app->active_game = game;
    uint8_t g = game;
    ha_proto_send(app->uart, HA_MSG_SELECT_GAME, &g, 1);
}

void ha_reset_scores(HotspotArcadeApp* app) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used) app->players[i].score = 0;
    ha_proto_send(app->uart, HA_MSG_RESET_SCORES, NULL, 0);
}

void ha_session_apply_language(HotspotArcadeApp* app) {
    if(!app || !app->session_active || !app->portal_running) return;
    app->content_refresh_pending = true;
    furi_string_set(app->status, "updating language");
    ha_content_stream_packs(app);
}

// ---------------- handshake / file streaming ----------------

static void send_config(HotspotArcadeApp* app) {
    FuriString* j = furi_string_alloc();
    // `lang` is the phone-UI language; the ESP echoes it to each phone in `welcome`.
    // The admission code is fixed-width decimal, so it needs no JSON escaping.
    furi_string_printf(
        j,
        "{\"max\":%d,\"lang\":\"%s\",\"code\":\"%s\"}",
        HA_MAX_PLAYERS < 8 ? HA_MAX_PLAYERS : 8,
        app->lang,
        app->join_code);
    ha_proto_send(
        app->uart, HA_MSG_CONFIG, (const uint8_t*)furi_string_get_cstr(j), furi_string_size(j));
    furi_string_free(j);
}

// Stream file asset[file_idx], or advance to SET_AP when all files are sent.
static void send_next_file(HotspotArcadeApp* app) {
    if(app->file_idx >= app->asset_count) {
        // All files streamed: stream the content packs (as votable topics), then
        // name the AP and start.
        ha_content_stream_packs(app);
        ha_proto_send_str(app->uart, HA_MSG_SET_AP, furi_string_get_cstr(app->ssid));
        app->hs = HaHsSetAp;
        return;
    }
    HaAsset* a = &app->assets[app->file_idx];
    FuriString* path = furi_string_alloc();
    furi_string_printf(path, "%s/%s", app->web_dir, a->file);
    FuriString* content = furi_string_alloc();
    bool ok = ha_storage_read_file(furi_string_get_cstr(path), content, HA_FILE_MAX);
    furi_string_free(path);
    if(!ok) {
        furi_string_set(app->status, "asset read err");
        app->hs = HaHsErr;
        furi_string_free(content);
        return;
    }
    size_t total = furi_string_size(content);

    // FILE_BEGIN payload: flags(1) pathlen(1) path mimelen(1) mime total(4 LE)
    uint8_t hdr[HA_ASSET_PATH + HA_ASSET_MIME + 8];
    size_t i = 0;
    hdr[i++] = a->gzip ? 1 : 0;
    size_t pl = strlen(a->path);
    hdr[i++] = (uint8_t)pl;
    memcpy(hdr + i, a->path, pl);
    i += pl;
    size_t ml = strlen(a->mime);
    hdr[i++] = (uint8_t)ml;
    memcpy(hdr + i, a->mime, ml);
    i += ml;
    hdr[i++] = (uint8_t)(total & 0xFF);
    hdr[i++] = (uint8_t)((total >> 8) & 0xFF);
    hdr[i++] = (uint8_t)((total >> 16) & 0xFF);
    hdr[i++] = (uint8_t)((total >> 24) & 0xFF);
    ha_proto_send(app->uart, HA_MSG_FILE_BEGIN, hdr, i);

    // Then the raw file bytes (unframed), as the protocol's bulk escape.
    ha_uart_tx(app->uart, (const uint8_t*)furi_string_get_cstr(content), total);
    furi_string_free(content);
}

static void start_handshake(HotspotArcadeApp* app) {
    app->last_handshake_tick = furi_get_tick();
    roster_detach_all(app);
    app->portal_running = false; // show progress, not "Broadcasting", while (re)streaming
    app->file_idx = 0;
    ha_storage_load_manifest(app);
    furi_string_set(app->status, "starting");
    // Discard stale bytes and reset the frame parser.
    app->rx_state = RXS_SYNC;
    uint8_t scratch[64];
    while(ha_uart_rx(app->uart, scratch, sizeof(scratch)) > 0) {
    }
    ha_proto_send(app->uart, HA_MSG_CLEAR_FILES, NULL, 0);
    app->hs = HaHsClear;
}

void ha_session_start(HotspotArcadeApp* app) {
    app->session_active = true;
    app->portal_running = false;
    app->link_lost = false;
    app->last_rx_tick = furi_get_tick();
    app->active_game = HA_GAME_NONE;
    app->content_refresh_pending = false;
    roster_clear(app);
    furi_string_reset(app->console);
    start_handshake(app);
}

void ha_session_stop(HotspotArcadeApp* app) {
    ha_proto_send(app->uart, HA_MSG_STOP, NULL, 0);
    app->session_active = false;
    app->portal_running = false;
    app->hs = HaHsIdle;
    furi_string_set(app->status, "stopped");
}

// ---------------- STATUS handling (drives the handshake) ----------------

static void on_status(HotspotArcadeApp* app, const char* tok) {
    furi_string_set(app->status, tok);
    // A valid framed STATUS means the board speaks our protocol: reset the
    // handshake watchdog. (Wrong/absent firmware never gets here, so the watchdog
    // in the tick fires and drops to the Install-firmware prompt.)
    if(app->hs > HaHsIdle && app->hs < HaHsUp) app->last_handshake_tick = furi_get_tick();
    if(strncmp(tok, "content_error", 13) == 0) {
        if(!app->content_refresh_pending) app->hs = HaHsErr;
        app->content_refresh_pending = false;
        furi_string_set(app->status, "content rejected");
    } else if(strncmp(tok, "content_ok", 10) == 0) {
        if(app->content_refresh_pending) {
            app->content_refresh_pending = false;
            furi_string_set(app->status, "language updated");
        }
    } else if(strncmp(tok, "cleared", 7) == 0) {
        if(app->hs == HaHsClear) {
            app->hs = HaHsFiles;
            app->file_idx = 0;
            send_next_file(app);
        }
    } else if(strncmp(tok, "fok", 3) == 0) {
        if(app->hs == HaHsFiles) {
            app->file_idx++;
            send_next_file(app);
        }
    } else if(strncmp(tok, "ap_set", 6) == 0) {
        if(app->hs == HaHsSetAp) {
            send_config(app);
            ha_proto_send(app->uart, HA_MSG_START, NULL, 0);
            app->hs = HaHsStart;
        }
    } else if(strncmp(tok, "up", 2) == 0) {
        app->portal_running = true;
        app->hs = HaHsUp;
        // Restore the selected game if the ESP had rebooted mid-session.
        if(app->active_game != HA_GAME_NONE) {
            uint8_t g = app->active_game;
            ha_proto_send(app->uart, HA_MSG_SELECT_GAME, &g, 1);
        }
    } else if(strncmp(tok, "stopped", 7) == 0) {
        app->portal_running = false;
    } else if(strncmp(tok, "boot", 4) == 0) {
        // ESP rebooted: it lost the AP + all clients. Redo the handshake, but
        // rate-limit so a board stuck rebooting can't tight-loop the handshake.
        if(app->session_active && (furi_get_tick() - app->last_handshake_tick) > 3000) {
            start_handshake(app);
        }
    }
}

// ---------------- frame dispatch ----------------

static void dispatch_frame(HotspotArcadeApp* app) {
    uint8_t* p = app->rx_buf;
    uint16_t len = app->rx_len;
    p[len] = '\0'; // JSON/text payloads: safe (buf has +1)

    // PING is the firmware's identity beacon: magic(4) + version(2 LE). Only a beacon
    // carrying OUR magic counts as "our board present" (so another project's firmware
    // on the same ESP isn't mistaken for ours); we also capture its version so an
    // outdated board can be flagged. Tracked even when idle.
    if(app->rx_type == HA_MSG_PING) {
        if(len >= 4 && p[0] == HA_FW_MAGIC_0 && p[1] == HA_FW_MAGIC_1 && p[2] == HA_FW_MAGIC_2 &&
           p[3] == HA_FW_MAGIC_3) {
            app->last_ping_tick = furi_get_tick();
            app->board_fw_version = (len >= 6) ? (uint16_t)(p[4] | ((uint16_t)p[5] << 8)) : 0;
        }
        return;
    }
    // Everything else (roster/scores/events) only matters during a live session.
    if(app->rx_type != HA_MSG_STATUS && !app->session_active) return;

    switch(app->rx_type) {
    case HA_MSG_STATUS:
        on_status(app, (const char*)p);
        break;
    case HA_MSG_JOIN:
        if(len >= 1 + HA_IDENTITY_BYTES) {
            static const char hex[] = "0123456789abcdef";
            char identity[HA_IDENTITY_BYTES * 2 + 1];
            for(size_t i = 0; i < HA_IDENTITY_BYTES; i++) {
                identity[i * 2] = hex[p[1 + i] >> 4];
                identity[i * 2 + 1] = hex[p[1 + i] & 15];
            }
            identity[HA_IDENTITY_BYTES * 2] = '\0';
            char nick[HA_NICK_LEN];
            size_t nl = len - 1 - HA_IDENTITY_BYTES < HA_NICK_LEN - 1 ?
                            (size_t)(len - 1 - HA_IDENTITY_BYTES) : HA_NICK_LEN - 1;
            memcpy(nick, p + 1 + HA_IDENTITY_BYTES, nl);
            nick[nl] = '\0';
            nick_upper(nick);
            player_join(app, p[0], identity, nick);
            FuriString* c = furi_string_alloc();
            furi_string_printf(c, "JOIN %s", nick);
            console_add(app, furi_string_get_cstr(c));
            furi_string_free(c);
            feedback_blip(app);
        }
        break;
    case HA_MSG_LEAVE:
        if(len >= 1) {
            player_leave(app, p[0]);
            console_add(app, "LEAVE");
        }
        break;
    case HA_MSG_SCORE:
        if(len >= 3) {
            int16_t d = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            player_score(app, p[0], d);
        }
        break;
    case HA_MSG_ROUND_RESULT:
        // Reserved for v17 boards. A v18 handshake never sends ad-hoc result JSON.
        break;
    case HA_MSG_EVENT: {
        char ev[160];
        bool success = false;
        if(format_host_event(app, p, len, ev, sizeof(ev), &success)) {
            furi_string_set_str(app->last_event, ev);
            console_add(app, ev);
            if(success) feedback_success(app);
        }
        break;
    }
    default:
        break;
    }
}

static void rx_byte(HotspotArcadeApp* app, uint8_t c) {
    switch(app->rx_state) {
    case RXS_SYNC:
        if(c == HA_SYNC) app->rx_state = RXS_TYPE;
        break;
    case RXS_TYPE:
        app->rx_type = c;
        app->rx_crc = ha_crc8_upd(0, c);
        app->rx_state = RXS_LEN0;
        break;
    case RXS_LEN0:
        app->rx_len = c;
        app->rx_crc = ha_crc8_upd(app->rx_crc, c);
        app->rx_state = RXS_LEN1;
        break;
    case RXS_LEN1:
        app->rx_len |= ((uint16_t)c << 8);
        app->rx_crc = ha_crc8_upd(app->rx_crc, c);
        if(app->rx_len > HA_MAX_PAYLOAD) {
            app->rx_state = RXS_SYNC;
            break;
        }
        app->rx_idx = 0;
        app->rx_state = app->rx_len ? RXS_PAYLOAD : RXS_CRC;
        break;
    case RXS_PAYLOAD:
        app->rx_buf[app->rx_idx++] = c;
        app->rx_crc = ha_crc8_upd(app->rx_crc, c);
        if(app->rx_idx >= app->rx_len) app->rx_state = RXS_CRC;
        break;
    case RXS_CRC:
        if(c == app->rx_crc) dispatch_frame(app);
        app->rx_state = RXS_SYNC;
        break;
    default:
        app->rx_state = RXS_SYNC;
        break;
    }
}

// ---------------- public RX / liveness ----------------

void ha_session_rx(HotspotArcadeApp* app) {
    uint8_t buf[128];
    size_t n;
    bool got = false;
    while((n = ha_uart_rx(app->uart, buf, sizeof(buf))) > 0) {
        for(size_t i = 0; i < n; i++)
            rx_byte(app, buf[i]);
        got = true;
    }
    if(got) {
        app->last_rx_tick = furi_get_tick();
        app->link_lost = false;
    }
}

// "Board present" means we heard our firmware's PING beacon recently. Parse the RX
// (rx_byte sets last_ping_tick on a valid framed PING), so a board running the wrong
// firmware, or nothing, fails this even though it may be spewing other bytes.
bool ha_board_present(HotspotArcadeApp* app, uint32_t wait_ms) {
    if(furi_get_tick() - app->last_ping_tick < 2500) return true;
    uint32_t deadline = furi_get_tick() + wait_ms;
    uint8_t buf[64];
    while(furi_get_tick() < deadline) {
        size_t n = ha_uart_rx(app->uart, buf, sizeof(buf));
        for(size_t i = 0; i < n; i++)
            rx_byte(app, buf[i]);
        if(furi_get_tick() - app->last_ping_tick < 2500) return true;
        if(n == 0) furi_delay_ms(20);
    }
    return false;
}
