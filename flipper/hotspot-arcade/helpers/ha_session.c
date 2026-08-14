#include "ha_session.h"
#include "ha_storage.h"
#include "../hotspot_arcade_i.h"
#include "../ha_json.h"
#include "ha_content_flow.h"
#include "ha_roster.h"

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

// A success cue for a typed win/draw/final milestone.
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

static const char* host_event_game_name(uint8_t game) {
    switch(game) {
    case HA_GAME_TRIVIA: return "Trivia";
    case HA_GAME_CONNECT4: return "Connect 4";
    case HA_GAME_TICTACTOE: return "Tic-Tac-Toe";
    case HA_GAME_DOTS: return "Dots & Boxes";
    case HA_GAME_DRAW: return "Drawing";
    case HA_GAME_PONG: return "Pong";
    case HA_GAME_REACT: return "Reaction Duel";
    case HA_GAME_WYR: return "Would You Rather";
    case HA_GAME_SCRAMBLE: return "Word Scramble";
    case HA_GAME_REVERSI: return "Reversi";
    case HA_GAME_GUESSCOLOR: return "Guess the Color";
    case HA_GAME_BATTLESHIP: return "Battleship";
    case HA_GAME_SPECTRUM: return "Spectrum";
    case HA_GAME_KMK: return "Kiss Marry Kill";
    case HA_GAME_CHESS: return "Chess";
    case HA_GAME_SECRETS: return "Secrets";
    case HA_GAME_FILLBLANK: return "Fill the Blank";
    case HA_GAME_WEREWOLF: return "Werewolf";
    case HA_GAME_SPYFALL: return "Spyfall";
    case HA_GAME_FRANKENDRAW: return "Draw a Monster";
    default: return "Arcade";
    }
}

static const char* host_event_player_name(HotspotArcadeApp* app, uint8_t pid) {
    int index = haRosterFind(app->players, pid);
    return index >= 0 ? app->players[index].nick : "?";
}

static void host_event_dispatch(HotspotArcadeApp* app, const uint8_t* payload, uint16_t len) {
    if(len < HA_HOST_EVENT_HEADER_SIZE || payload[0] != HA_HOST_EVENT_VERSION) return;
    uint8_t kind = payload[1], game = payload[2], actor = payload[3], target = payload[4];
    // Game 0 is the party lobby before the first content selection; CHAT events
    // there are valid and are formatted with the "Arcade" fallback name.
    if(game > HA_GAME_FRANKENDRAW || actor > HA_MAX_PLAYERS || target > HA_MAX_PLAYERS)
        return;
    int16_t value = (int16_t)((uint16_t)payload[5] | ((uint16_t)payload[6] << 8));
    size_t text_len = len - HA_HOST_EVENT_HEADER_SIZE;
    if(text_len > HA_HOST_EVENT_TEXT_MAX) return;
    if(text_len && !haContentFileBytesValid(payload + HA_HOST_EVENT_HEADER_SIZE, text_len)) return;
    for(size_t i = 0; i < text_len; i++)
        if(payload[HA_HOST_EVENT_HEADER_SIZE + i] < 0x20) return;
    char detail[HA_HOST_EVENT_TEXT_MAX + 1];
    if(text_len) memcpy(detail, payload + HA_HOST_EVENT_HEADER_SIZE, text_len);
    detail[text_len] = '\0';
    const char* game_name = host_event_game_name(game);
    const char* actor_name = host_event_player_name(app, actor);
    const char* target_name = host_event_player_name(app, target);
    FuriString* line = furi_string_alloc();
    bool status = true;
    switch(kind) {
    case HA_HOST_EVT_MATCH_STARTED:
        furi_string_printf(line, "%s: %s vs %s", game_name, actor_name, target_name);
        break;
    case HA_HOST_EVT_CHAT:
        furi_string_printf(line, "%s: %s", actor_name, detail);
        status = false;
        break;
    case HA_HOST_EVT_ROLE:
        furi_string_printf(line, "%s: %s %s", game_name, actor_name, detail);
        break;
    case HA_HOST_EVT_ROUND_WIN:
        if(detail[0])
            furi_string_printf(
                line, "%s: %s beat %s (%s)", game_name, actor_name, target_name, detail);
        else
            furi_string_printf(line, "%s: %s beat %s", game_name, actor_name, target_name);
        break;
    case HA_HOST_EVT_ROUND_DRAW:
        if(detail[0])
            furi_string_printf(
                line, "%s: %s / %s draw (%s)", game_name, actor_name, target_name, detail);
        else
            furi_string_printf(line, "%s: %s / %s draw", game_name, actor_name, target_name);
        break;
    case HA_HOST_EVT_ROUND_COMPLETE:
        if(detail[0])
            furi_string_printf(line, "%s: round %d complete (%s)", game_name, value, detail);
        else
            furi_string_printf(line, "%s: round %d complete", game_name, value);
        break;
    case HA_HOST_EVT_GAME_FINAL:
        if(detail[0])
            furi_string_printf(line, "%s: game complete (%s)", game_name, detail);
        else
            furi_string_printf(line, "%s: game complete", game_name);
        break;
    default:
        furi_string_free(line);
        return;
    }
    console_add(app, furi_string_get_cstr(line));
    if(status) {
        furi_string_set(app->last_event, line);
        if(kind == HA_HOST_EVT_ROUND_WIN || kind == HA_HOST_EVT_ROUND_DRAW ||
           kind == HA_HOST_EVT_GAME_FINAL)
            feedback_success(app);
    }
    furi_string_free(line);
}

// ---------------- roster ----------------

int ha_player_count(HotspotArcadeApp* app) {
    int n = 0;
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used) n++;
    return n;
}

static bool player_join(HotspotArcadeApp* app, uint8_t pid, const char* nick) {
    bool fresh = haRosterFind(app->players, pid) < 0;
    return haRosterUpsert(app->players, pid, nick) && fresh;
}

static void player_leave(HotspotArcadeApp* app, uint8_t pid) {
    int idx = haRosterFind(app->players, pid);
    if(idx >= 0) app->players[idx].used = false;
}

static void player_score(HotspotArcadeApp* app, uint8_t pid, int delta) {
    int idx = haRosterFind(app->players, pid);
    if(idx >= 0) app->players[idx].score += delta;
}

static void roster_clear(HotspotArcadeApp* app) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        app->players[i].used = false;
}

// ---------------- trivia pack streaming ----------------
// The Flipper no longer hosts trivia; it just streams every pack on the SD card to
// the ESP as votable topics when Trivia is selected, and the ESP orchestrates the game.

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

static bool content_stream_pack(
    HotspotArcadeApp* app,
    uint8_t game,
    const char* content,
    const char* fallback,
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
    furi_string_free(name);

    // Pass two: blocks.
    FuriString* obj = furi_string_alloc_set_str("{");
    FuriString* key = furi_string_alloc();
    FuriString* val = furi_string_alloc();
    bool any = false;
    bool valid = true;
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
                if(!furi_string_size(key)) {
                    valid = false;
                } else if(strcmp(k, "pack") != 0) {
                    if(any) furi_string_cat_str(obj, ",");
                    furi_string_cat_str(obj, "\"");
                    json_escape_cat(obj, k);
                    furi_string_cat_str(obj, "\":\"");
                    json_escape_cat(obj, furi_string_get_cstr(val));
                    furi_string_cat_str(obj, "\"");
                    any = true;
                }
            } else {
                // Non-empty records must be Key: value lines. We may already have
                // streamed earlier blocks, but the caller will abort the staging bank,
                // so malformed input can never publish a valid-looking prefix.
                valid = false;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    content_send_item(app, obj, &any, item_count);
    furi_string_free(obj);
    furi_string_free(key);
    furi_string_free(val);
    return valid;
}

#define HA_MAX_TOPICS (8) // must match TRIVIA_MAX_TOPICS on the ESP (raised from 6 in v19)

// Stream every .txt pack in one dir as votable topics, skipping names already streamed.
// `seen` holds the filenames taken so far; *topics is the running total across dirs.
static bool ha_content_stream_dir(
    HotspotArcadeApp* app,
    Storage* storage,
    const char* dir_path,
    uint8_t game,
    char seen[HA_MAX_TOPICS][80],
    int* topics,
    int pack_cap,
    uint16_t* item_count) {
    bool ok = true;
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, dir_path)) {
        FileInfo info;
        char name[80];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
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
            if(*topics >= pack_cap) {
                // Keep scanning semantics fail-closed: never silently ignore a valid
                // unique pack merely because the typed bank's pack cap was reached.
                ok = false;
                break;
            }
            FuriString* path = furi_string_alloc();
            furi_string_printf(path, "%s/%s", dir_path, name);
            FuriString* content = furi_string_alloc();
            bool read_ok =
                ha_storage_read_file(furi_string_get_cstr(path), content, HA_PACK_FILE_MAX + 1U);
            size_t content_size = furi_string_size(content);
            bool pack_ok = read_ok && info.size == (uint64_t)content_size &&
                           haContentFileBytesValid(
                               (const uint8_t*)furi_string_get_cstr(content), content_size);
            if(pack_ok) {
                FuriString* fb = furi_string_alloc_set_str(name);
                furi_string_left(fb, nl - 4); // drop ".txt"
                pack_ok = content_stream_pack(
                    app, game, furi_string_get_cstr(content), furi_string_get_cstr(fb), item_count);
                furi_string_free(fb);
                if(pack_ok) {
                    strlcpy(seen[*topics], name, sizeof(seen[0]));
                    (*topics)++;
                }
            }
            furi_string_free(content);
            furi_string_free(path);
            if(!pack_ok) {
                ok = false;
                break;
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    return ok;
}

// Stream one game's packs from packs/<sub> (user before bundled), into its per-game cap.
// `sub` is the game dir, optionally with a "/<lang>" suffix for a translated set.
static bool ha_stream_subdir(
    HotspotArcadeApp* app,
    Storage* storage,
    uint8_t game,
    const char* sub,
    char seen[HA_MAX_TOPICS][80],
    int* topics,
    int pack_cap,
    uint16_t* item_count) {
    FuriString* d = furi_string_alloc();
    furi_string_printf(d, "%s/%s", HA_USER_PACKS_DIR, sub);
    bool ok = ha_content_stream_dir(
        app, storage, furi_string_get_cstr(d), game, seen, topics, pack_cap, item_count);
    if(ok) {
        furi_string_printf(d, "%s/%s", HA_BUNDLED_PACKS_DIR, sub);
        ok = ha_content_stream_dir(
            app, storage, furi_string_get_cstr(d), game, seen, topics, pack_cap, item_count);
    }
    furi_string_free(d);
    return ok;
}

static const char* content_game_subdir(uint8_t game) {
    switch(game) {
    case HA_GAME_TRIVIA:
        return "trivia";
    case HA_GAME_WYR:
        return "wyr";
    case HA_GAME_SCRAMBLE:
        return "scramble";
    case HA_GAME_DRAW:
        return "draw";
    case HA_GAME_SPECTRUM:
        return "spectrum";
    case HA_GAME_KMK:
        return "kmk";
    case HA_GAME_SECRETS:
        return "secrets";
    case HA_GAME_FILLBLANK:
        return "fillblank";
    case HA_GAME_SPYFALL:
        return "spyfall";
    default:
        return NULL; // packless game: zero/zero is the complete transaction
    }
}

// Stream only the target game's content. The BEGIN/COMMIT pair makes selection and
// locale replacement atomic on the ESP: until the final count-checked commit succeeds,
// the previous game, round, scores, identities, and reconnect deadlines remain live.
static bool ha_content_stream_game(HotspotArcadeApp* app, uint8_t game) {
    // Mark the target before BEGIN so even an immediate error from any transaction
    // frame is correlated to this request. Callers serialize requests with this flag.
    app->pending_game = game;
    app->content_pending = true;
    uint8_t begin[1 + sizeof(app->lang) - 1];
    begin[0] = game;
    size_t lang_len = strlen(app->lang);
    if(lang_len > sizeof(begin) - 1) lang_len = sizeof(begin) - 1;
    memcpy(begin + 1, app->lang, lang_len);
    ha_proto_send(app->uart, HA_MSG_CONTENT_BEGIN, begin, 1 + lang_len);

    uint16_t item_count = 0;
    int topics = 0;
    bool ok = true;
    const char* sub = content_game_subdir(game);
    if(sub) {
        int pack_cap = (game == HA_GAME_FILLBLANK || game == HA_GAME_SPYFALL) ? 3 : HA_MAX_TOPICS;
        char seen[HA_MAX_TOPICS][80] = {{0}};
        Storage* storage = furi_record_open(RECORD_STORAGE);

        // Prefer the selected locale, falling back transactionally to the English root.
        if(app->lang[0]) {
            FuriString* localized = furi_string_alloc();
            furi_string_printf(localized, "%s/%s", sub, app->lang);
            ok = ha_stream_subdir(
                app,
                storage,
                game,
                furi_string_get_cstr(localized),
                seen,
                &topics,
                pack_cap,
                &item_count);
            furi_string_free(localized);
        }
        if(ok && topics == 0)
            ok = ha_stream_subdir(app, storage, game, sub, seen, &topics, pack_cap, &item_count);

        // Preserve support for the pre-packs/ trivia layout on the English fallback.
        if(ok && game == HA_GAME_TRIVIA && (!app->lang[0] || topics == 0)) {
            ok = ha_content_stream_dir(
                app, storage, HA_USER_TRIVIA_DIR, game, seen, &topics, pack_cap, &item_count);
            if(ok)
                ok = ha_content_stream_dir(
                    app, storage, HA_BUNDLED_TRIVIA_DIR, game, seen, &topics, pack_cap, &item_count);
        }
        furi_record_close(RECORD_STORAGE);
    }

    if(!ok) {
        ha_proto_send(app->uart, HA_MSG_CONTENT_ABORT, NULL, 0);
        app->content_pending = false;
        furi_string_set(app->status, "content_file_error");
        return false;
    }

    uint8_t commit[4] = {
        (uint8_t)(topics & 0xFF),
        (uint8_t)((uint16_t)topics >> 8),
        (uint8_t)(item_count & 0xFF),
        (uint8_t)(item_count >> 8),
    };
    ha_proto_send(app->uart, HA_MSG_CONTENT_COMMIT, commit, sizeof(commit));
    return true;
}

// ---------------- game selection ----------------

void ha_select_game(HotspotArcadeApp* app, uint8_t game) {
    if(app->content_pending) {
        furi_string_set(app->status, "content_busy");
        return;
    }
    (void)ha_content_stream_game(app, game);
}

void ha_reset_scores(HotspotArcadeApp* app) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(app->players[i].used) app->players[i].score = 0;
    ha_proto_send(app->uart, HA_MSG_RESET_SCORES, NULL, 0);
}

// ---------------- handshake / file streaming ----------------

static void send_config(HotspotArcadeApp* app) {
    FuriString* j = furi_string_alloc();
    // Locale belongs to CONTENT_BEGIN/COMMIT; CONFIG cannot publish it ahead
    // of a replacement transaction that may still fail.
    furi_string_printf(j, "{\"max\":%d}", HA_MAX_PLAYERS < 8 ? HA_MAX_PLAYERS : 8);
    ha_proto_send(
        app->uart, HA_MSG_CONFIG, (const uint8_t*)furi_string_get_cstr(j), furi_string_size(j));
    furi_string_free(j);
}

// Stream file asset[file_idx], or advance to the content-restore gate when complete.
static void send_next_file(HotspotArcadeApp* app) {
    if(app->file_idx >= app->asset_count) {
        // All files streamed: restore just the selected game's content bank and wait
        // for its exact correlated acknowledgement before naming/starting the AP. On
        // a new session this is NONE (a valid zero/zero bank).
        app->hs = HaHsContent;
        if(!ha_content_stream_game(app, app->active_game)) app->hs = HaHsErr;
        return;
    }
    HaAsset* a = &app->assets[app->file_idx];
    FuriString* path = furi_string_alloc();
    furi_string_printf(path, "%s/%s", app->web_dir, a->file);
    FuriString* content = furi_string_alloc();
    // Read one byte past the cap so an oversized file is DETECTED, not silently
    // truncated: a clipped gzip stream serves a page whose tail (all the scripts)
    // never arrives, which looks like "the app is broken" on every phone with no
    // error anywhere. Better to refuse loudly here.
    bool ok = ha_storage_read_file(furi_string_get_cstr(path), content, HA_FILE_MAX + 1);
    furi_string_free(path);
    if(!ok) {
        furi_string_set(app->status, "asset read err");
        app->hs = HaHsErr;
        furi_string_free(content);
        return;
    }
    if(furi_string_size(content) > HA_FILE_MAX) {
        furi_string_set(app->status, "web asset too big");
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
    roster_clear(app);
    app->portal_running = false; // show progress, not "Broadcasting", while (re)streaming
    app->file_idx = 0;
    app->pending_game = HA_GAME_NONE;
    app->content_pending = false;
    ha_storage_load_manifest(app);
    furi_string_set(app->status, "starting");
    // Discard stale bytes and reset the frame parser.
    app->rx_state = RXS_SYNC;
    uint8_t scratch[64];
    while(ha_uart_rx(app->uart, scratch, sizeof(scratch)) > 0) {
    }
    if(app->web_bundle_crc != 0 && app->web_bundle_crc == app->board_bundle_crc) {
        // The ESP already holds this exact bundle in flash (CRC from its PING beacon):
        // skip CLEAR_FILES and the whole file stream, go straight to the content gate. We
        // must NOT send CLEAR_FILES here, or the ESP would wipe the bundle we're relying on.
        app->file_idx = app->asset_count;
        send_next_file(app); // streams content packs, waits in HaHsContent for exact ACK
        return;
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
    app->transport_paused = false;
    app->transport_network_ready = false;
    app->transport_wait_expired = false;
    app->transport_reason = 0;
    app->transport_expected_mask = 0;
    app->transport_online_mask = 0;
    app->transport_reconnect_ms = 0;
    app->transport_host_deadline_set = false;
    app->transport_host_deadline = 0;
    app->transport_pending_ssid[0] = '\0';
    app->pending_game = HA_GAME_NONE;
    app->content_pending = false;
    furi_string_reset(app->console);
    start_handshake(app);
}

void ha_session_stop(HotspotArcadeApp* app) {
    ha_art_abort(app); // no half-written SVG survives the session
    ha_proto_send(app->uart, HA_MSG_STOP, NULL, 0);
    app->session_active = false;
    app->portal_running = false;
    app->transport_paused = false;
    app->transport_network_ready = false;
    app->transport_wait_expired = false;
    app->transport_reason = 0;
    app->transport_expected_mask = 0;
    app->transport_online_mask = 0;
    app->transport_reconnect_ms = 0;
    app->transport_host_deadline_set = false;
    app->transport_host_deadline = 0;
    app->transport_pending_ssid[0] = '\0';
    app->hs = HaHsIdle;
    furi_string_set(app->status, "stopped");
}

void ha_session_transport_pause(
    HotspotArcadeApp* app, uint8_t reason, const char* ssid, uint32_t reconnect_ms) {
    if(!app || !app->session_active || app->hs != HaHsUp || !app->portal_running ||
       !app->transport_network_ready || app->transport_paused || reconnect_ms > 600000)
        return;
    FuriString* json = furi_string_alloc_set_str("{\"reason\":\"");
    furi_string_cat_str(
        json, reason == HA_TRANSPORT_SSID_CHANGE ? "ssid_change" : "ap_off");
    furi_string_cat_str(json, "\",\"ssid\":\"");
    json_escape_cat(json, ssid ? ssid : "");
    furi_string_cat_printf(json, "\",\"reconnect_ms\":%lu}", (unsigned long)reconnect_ms);
    ha_proto_send(
        app->uart,
        HA_MSG_TRANSPORT_PAUSE,
        (const uint8_t*)furi_string_get_cstr(json),
        furi_string_size(json));
    furi_string_free(json);
    app->transport_reason = reason;
    app->transport_reconnect_ms = reconnect_ms;
    app->transport_host_deadline_set = false;
    app->transport_host_deadline = 0; // starts only after the network is healthy again
    app->transport_network_ready = false; // the 200 ms server_pause flush is not healthy
    app->transport_wait_expired = false;
    furi_string_set(app->status, "transport_pausing");
}

void ha_session_network_restart(HotspotArcadeApp* app) {
    if(!app || !app->session_active || !app->transport_paused || app->portal_running) return;
    app->transport_network_ready = false;
    furi_string_set(app->status, "network_starting");
    ha_proto_send(app->uart, HA_MSG_START, NULL, 0);
}

void ha_session_transport_resume(HotspotArcadeApp* app) {
    if(!app || !app->session_active || !app->transport_paused || !app->portal_running ||
       !app->transport_network_ready)
        return;
    // RESUME is idempotent at the engine. Permit an explicit retry if a UART frame
    // or acknowledgement is lost instead of leaving the host stuck forever in a
    // locally optimistic "transport_resuming" state.
    furi_string_set(app->status, "transport_resuming");
    if(app->transport_wait_expired) {
        const uint8_t flags = HA_TRANSPORT_RESUME_EXPIRE_MISSING;
        ha_proto_send(app->uart, HA_MSG_TRANSPORT_RESUME, &flags, 1);
    } else {
        ha_proto_send(app->uart, HA_MSG_TRANSPORT_RESUME, NULL, 0);
    }
}

bool ha_session_transport_wait_elapsed(const HotspotArcadeApp* app) {
    return app && app->transport_host_deadline_set &&
           (int32_t)(furi_get_tick() - app->transport_host_deadline) >= 0;
}

// ---------------- STATUS handling (drives the handshake) ----------------

static void on_status(HotspotArcadeApp* app, const char* tok) {
    furi_string_set(app->status, tok);
    // A valid framed STATUS means the board speaks our protocol: reset the
    // handshake watchdog. (Wrong/absent firmware never gets here, so the watchdog
    // in the tick fires and drops to the Install-firmware prompt.)
    if(app->hs > HaHsIdle && app->hs < HaHsUp) app->last_handshake_tick = furi_get_tick();
    if(strncmp(tok, "cleared", 7) == 0) {
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
            app->hs = HaHsStart;
            ha_proto_send(app->uart, HA_MSG_START, NULL, 0);
        }
    } else if(strncmp(tok, "content_ok", 10) == 0) {
        uint8_t committed_game = HA_GAME_NONE;
        bool matches =
            haContentStatusGame(tok, "content_ok", HA_GAME_FRANKENDRAW, &committed_game) &&
            app->content_pending && committed_game == app->pending_game;
        if(!matches) {
            // Leave a newer transaction pending; stale/unidentified ACKs cannot
            // advance the boot handshake or overwrite the displayed active game.
            furi_string_set(app->status, "content_wait");
            return;
        }
        app->active_game = committed_game;
        app->content_pending = false;
        if(app->hs == HaHsContent) {
            app->hs = HaHsSetAp;
            ha_proto_send_str(app->uart, HA_MSG_SET_AP, furi_string_get_cstr(app->ssid));
        }
    } else if(strncmp(tok, "content_error", 13) == 0) {
        uint8_t failed_game = HA_GAME_NONE;
        bool matches =
            haContentStatusGame(tok, "content_error", HA_GAME_FRANKENDRAW, &failed_game) &&
            app->content_pending && failed_game == app->pending_game;
        if(!matches) {
            furi_string_set(app->status, "content_wait");
            return;
        }
        // The ESP preserved the previous live bank. During startup there is no safe
        // fallback after reboot, so fail closed instead of exposing a NONE lobby.
        app->content_pending = false;
        if(app->hs == HaHsContent) app->hs = HaHsErr;
    } else if(strncmp(tok, "up", 2) == 0) {
        app->portal_running = true;
        // STATUS up is immediately followed by TRANSPORT_STATE. Do not use stale masks
        // from before shutdown to auto-resume in this gap; only the fresh snapshot may
        // mark the restarted network ready and start the host reconnect window.
        app->transport_network_ready = false;
        app->transport_host_deadline_set = false;
        app->transport_host_deadline = 0;
        if(app->transport_paused && app->transport_reason == HA_TRANSPORT_SSID_CHANGE &&
           app->transport_pending_ssid[0]) {
            furi_string_set(app->ssid, app->transport_pending_ssid);
            ha_storage_save_config(app);
            app->transport_pending_ssid[0] = '\0';
        }
        app->hs = HaHsUp;
        // The handshake already restored selection through CONTENT_BEGIN..COMMIT.
    } else if(strncmp(tok, "stopped", 7) == 0) {
        app->portal_running = false;
    } else if(strncmp(tok, "network_suspended", 17) == 0) {
        app->portal_running = false;
        app->transport_network_ready = false;
        app->transport_host_deadline_set = false;
        app->transport_host_deadline = 0;
        if(app->transport_reason == HA_TRANSPORT_SSID_CHANGE)
            ha_session_network_restart(app);
    } else if(strncmp(tok, "ap_fallback", 11) == 0 ||
              ((strncmp(tok, "ap_error", 8) == 0 || strncmp(tok, "dns_error", 9) == 0) &&
               app->transport_paused && app->transport_reason == HA_TRANSPORT_SSID_CHANGE)) {
        // The old app->ssid was intentionally kept until success. AP/DNS failure
        // precedes `ap_fallback`, so either framed signal independently discards
        // the candidate; a lost fallback token cannot let the later old-SSID `up`
        // accidentally persist the failed name.
        app->transport_pending_ssid[0] = '\0';
    } else if(strncmp(tok, "transport_error", 15) == 0 ||
              strncmp(tok, "transport_conflict", 18) == 0) {
        app->transport_pending_ssid[0] = '\0';
    } else if(strncmp(tok, "transport_resumed", 17) == 0) {
        app->transport_paused = false;
        app->transport_wait_expired = false;
        app->transport_expected_mask = 0;
        app->transport_online_mask = 0;
        app->transport_host_deadline_set = false;
        app->transport_host_deadline = 0;
        furi_string_set(app->status, "up");
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
            // v19+: bytes 6-9 carry the CRC32 of the web bundle the ESP holds in flash, so
            // we can skip re-streaming it when it already matches ours. Older boards omit it.
            app->board_bundle_crc =
                (len >= 10) ? (uint32_t)((uint32_t)p[6] | ((uint32_t)p[7] << 8) |
                                         ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24))
                            : 0;
            // v19+: byte 10 is the ESP's last committed game id. Mirror it as a
            // recovery/diagnostic backstop. Skip 0 (NONE): the ESP reports NONE for a beat
            // after a reboot, before the "up" handler re-pushes the game to restore it -- don't
            // clobber the game we're about to restore.
            if(len >= 11 && app->session_active && p[10] != 0 && p[10] != app->active_game)
                app->active_game = p[10];
            // v1.7.1+: bytes 11-14 are the ESP's free internal heap and free PSRAM in KB (LE
            // uint16 each), for the dashboard memory readout. Older boards omit them -> 0.
            app->board_heap_kb = (len >= 13) ? (uint16_t)(p[11] | ((uint16_t)p[12] << 8)) : 0;
            app->board_psram_kb = (len >= 15) ? (uint16_t)(p[13] | ((uint16_t)p[14] << 8)) : 0;
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
        if(len >= 1) {
            char nick[HA_NICK_LEN];
            size_t nl = len - 1 < HA_NICK_LEN - 1 ? (size_t)(len - 1) : HA_NICK_LEN - 1;
            memcpy(nick, p + 1, nl);
            nick[nl] = '\0';
            // Stable JOIN is an idempotent upsert. Only a genuinely new seat is
            // a host-visible arrival; resume/takeover/profile updates must not
            // spam the console or vibration cue on a flaky connection.
            if(player_join(app, p[0], nick)) {
                int idx = haRosterFind(app->players, p[0]);
                const char* shown = idx >= 0 ? app->players[idx].nick : nick;
                FuriString* c = furi_string_alloc();
                furi_string_printf(c, "JOIN %s", shown);
                console_add(app, furi_string_get_cstr(c));
                furi_string_free(c);
                feedback_blip(app);
            }
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
        // Legacy v21-and-older JSON result. Current v22 firmware emits only the
        // bounded typed HA_MSG_EVENT frame below.
        break;
    case HA_MSG_ART:
        // Finished Frankendraw artwork: op byte + JSON. Straight through to the SVG
        // writer -- a segment at a time, nothing held between frames.
        if(len >= 1) {
            const char* js = (const char*)p + 1;
            if(p[0] == HA_ART_BEGIN)
                ha_art_begin(app, js);
            else if(p[0] == HA_ART_STROKE)
                ha_art_stroke(app, js);
            else if(p[0] == HA_ART_END)
                ha_art_end(app);
        }
        break;
    case HA_MSG_TRANSPORT_STATE:
        if(len == 10) {
            bool was_network_ready = app->transport_network_ready;
            app->portal_running = (p[0] & 0x08) != 0;
            app->transport_paused = (p[0] & 0x01) != 0;
            app->transport_network_ready = (p[0] & 0x02) != 0;
            app->transport_reason = p[1];
            app->transport_expected_mask = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
            app->transport_online_mask = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            app->transport_reconnect_ms = (uint32_t)p[6] | ((uint32_t)p[7] << 8) |
                                          ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
            // A periodic snapshot may describe an old still-live portal while a
            // fresh handshake is streaming files/content. Only START is waiting
            // for portal confirmation; never skip an in-flight handshake phase.
            if(app->portal_running && app->transport_network_ready && app->hs == HaHsStart)
                app->hs = HaHsUp;
            if(app->transport_paused && app->portal_running &&
               app->transport_network_ready &&
               app->transport_reason == HA_TRANSPORT_SSID_CHANGE &&
               app->transport_pending_ssid[0]) {
                // A healthy authoritative snapshot is a redundant replacement for
                // a dropped `up` STATUS. Failure tokens clear this candidate before
                // a fallback snapshot can arrive.
                furi_string_set(app->ssid, app->transport_pending_ssid);
                ha_storage_save_config(app);
                app->transport_pending_ssid[0] = '\0';
            }
            // TRANSPORT_STATE is authoritative even if the preceding STATUS frame was
            // lost or failed its CRC. In particular, a successful explicit resume must
            // dismiss the ten-minute Resume/End prompt without depending on the
            // best-effort `transport_resumed` token.
            if(!app->transport_paused) {
                app->transport_wait_expired = false;
                app->transport_expected_mask = 0;
                app->transport_online_mask = 0;
                app->transport_reason = 0;
                app->transport_reconnect_ms = 0;
                app->transport_host_deadline_set = false;
                app->transport_host_deadline = 0;
                if(app->portal_running) furi_string_set(app->status, "up");
            }
            if(app->transport_paused && app->portal_running &&
               app->transport_network_ready && !was_network_ready &&
               app->transport_reconnect_ms) {
                app->transport_host_deadline = furi_get_tick() + app->transport_reconnect_ms;
                app->transport_host_deadline_set = true;
            }
            if(app->transport_paused && !app->transport_wait_expired &&
               ha_session_transport_wait_elapsed(app)) {
                app->transport_wait_expired = true;
                furi_string_set(app->status, "transport_wait_expired");
            }
            if(app->transport_paused && app->portal_running &&
               app->transport_network_ready && !app->transport_wait_expired &&
               app->transport_online_mask == app->transport_expected_mask)
                ha_session_transport_resume(app);
            else if(app->transport_paused && !app->portal_running &&
                    app->transport_reason == HA_TRANSPORT_SSID_CHANGE)
                // The down snapshot recovers a dropped `network_suspended` STATUS.
                ha_session_network_restart(app);
        }
        break;
    case HA_MSG_EVENT: {
        host_event_dispatch(app, p, len);
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
