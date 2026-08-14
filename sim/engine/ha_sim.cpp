// Hosts the REAL engine (esp32/hotspot-arcade-fw/ha_games.h) off-target.
//
// The engine reaches the outside world only through 9 sink functions, which the
// firmware implements against AsyncWebServer and the UART. Here they append to an
// outbox queue instead, and ha_drain() hands it to the harness as JSON. That queue
// is the fidelity boundary: it carries exactly what the firmware would have sent.
#include "Arduino.h"

#include <algorithm>
#include <string>
#include <vector>

static uint32_t g_millis = 0;
uint32_t millis() { return g_millis; }

// Exposes the chess rules-core and match test hooks (chessPerft/chessLoadCore,
// chessTestLoad/chessTestPerft) so the sim can drive positions the normal opening
// moves can't reach quickly (mate/stalemate/draw setups, perft ground truth).
#define HA_CHESS_TEST
#define HA_ENABLE_MIN_OVERRIDE 1
#include "../../esp32/hotspot-arcade-fw/ha_games.h"

static Engine engine;
static std::vector<std::string> g_outbox;
static std::vector<std::string> g_knownIdentities;
static bool g_admissionFull = false;
static std::string g_drained; // return buffer; must outlive the call

// Escape a C string for embedding as a JSON string value. Only nicknames and score
// reasons need this; every other payload is already JSON text and is spliced raw.
static std::string esc(const char* s) {
    std::string o;
    for(const char* p = s ? s : ""; *p; p++) {
        switch(*p) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if((unsigned char)*p < 0x20) {
                char b[7];
                snprintf(b, sizeof(b), "\\u%04x", *p);
                o += b;
            } else {
                o += *p;
            }
        }
    }
    return o;
}

// --- the 9 sinks ---------------------------------------------------------------
// msg/json arguments are already valid JSON objects, so they are spliced in raw
// rather than escaped into a string. That keeps the drained payload directly
// usable as structured data on the JS side.

void haWsSendWs(uint32_t wsId, const String& msg) {
    g_outbox.push_back(
        "{\"to\":\"ws\",\"id\":" + std::to_string(wsId) + ",\"msg\":" + msg.str() + "}");
}

void haWsCloseWs(uint32_t wsId) {
    g_outbox.push_back(
        "{\"to\":\"ws\",\"id\":" + std::to_string(wsId) +
        ",\"kind\":\"close\",\"code\":1008,\"reason\":\"identity takeover\"}");
}

void haWsBroadcast(const String& msg) {
    g_outbox.push_back("{\"to\":\"all\",\"msg\":" + msg.str() + "}");
}

uint8_t haAuthorizeIdentity(
    uint32_t wsId, const char* identity, const char* code, uint32_t* retryMs) {
    (void)wsId;
    if(retryMs) *retryMs = 0;
    if(std::find(g_knownIdentities.begin(), g_knownIdentities.end(), identity) !=
       g_knownIdentities.end())
        return HA_JOIN_AUTH_KNOWN;
    if(g_admissionFull) return HA_JOIN_AUTH_FULL;
    if(!code || !code[0]) return HA_JOIN_AUTH_REQUIRED;
    return strcmp(code, "123456") == 0 ? HA_JOIN_AUTH_OK : HA_JOIN_AUTH_BAD_CODE;
}

void haUartJoin(uint8_t pid, const char* nick) {
    g_outbox.push_back(
        "{\"to\":\"uart\",\"kind\":\"join\",\"pid\":" + std::to_string(pid) +
        ",\"nick\":\"" + esc(nick) + "\"}");
}

void haUartJoinStable(uint8_t pid, const char* identity, const char* nick, const char* avatar) {
    if(std::find(g_knownIdentities.begin(), g_knownIdentities.end(), identity) ==
       g_knownIdentities.end())
        g_knownIdentities.push_back(identity);
    g_outbox.push_back(
        "{\"to\":\"uart\",\"kind\":\"join\",\"pid\":" + std::to_string(pid) +
        ",\"identity\":\"" + esc(identity) + "\",\"nick\":\"" + esc(nick) +
        "\",\"avatar\":\"" + esc(avatar) + "\"}");
}

void haUartLeave(uint8_t pid) {
    g_outbox.push_back(
        "{\"to\":\"uart\",\"kind\":\"leave\",\"pid\":" + std::to_string(pid) + "}");
}

void haUartScore(uint8_t pid, int delta, const char* reason) {
    g_outbox.push_back(
        "{\"to\":\"uart\",\"kind\":\"score\",\"pid\":" + std::to_string(pid) +
        ",\"delta\":" + std::to_string(delta) + ",\"reason\":\"" + esc(reason) + "\"}");
}

void haUartEvent(const String& json) {
    g_outbox.push_back("{\"to\":\"uart\",\"kind\":\"event\",\"json\":" + json.str() + "}");
}

void haUartRoundResult(const String& json) {
    g_outbox.push_back("{\"to\":\"uart\",\"kind\":\"round\",\"json\":" + json.str() + "}");
}

// The 8th sink: the identity trace the firmware prints to its serial console. It is
// not protocol, but it is the one place that says WHY a hello became a new player or
// was consolidated onto an existing one, so the sim surfaces it as its own outbox
// kind rather than dropping it (routing ignores unknown `to` values).
void haLogJoin(uint8_t pid, uint64_t deviceKey, const char* nick, bool consolidated) {
    g_outbox.push_back(
        std::string("{\"to\":\"log\",\"kind\":\"") + (consolidated ? "consolidated" : "join") +
        "\",\"pid\":" + std::to_string(pid) + ",\"device\":" + std::to_string(deviceKey) +
        ",\"nick\":\"" + esc(nick) + "\"}");
}

// The 9th sink: a finished Frankendraw sheet, streamed as begin / one frame per line
// segment / end. On hardware the Flipper turns that stream into an SVG on its SD card;
// here each call becomes one outbox item, so a test can assert exactly what was saved.
void haUartArt(uint8_t op, const String& json) {
    g_outbox.push_back(
        "{\"to\":\"uart\",\"kind\":\"art\",\"op\":" + std::to_string((int)op) +
        ",\"json\":" + json.str() + "}");
}

// --- exported C API ------------------------------------------------------------
extern "C" {

void ha_reset() {
    g_millis = 0;
    g_knownIdentities.clear();
    g_admissionFull = false;
    engine.reset(g_millis);
}
void ha_reset_at(uint32_t now) {
    g_millis = now;
    g_knownIdentities.clear();
    g_admissionFull = false;
    engine.reset(g_millis);
}
void ha_set_admission_full(int full) { g_admissionFull = full != 0; }

void ha_tick(uint32_t now) {
    g_millis = now;
    engine.tick(now);
}

void ha_input(uint32_t wsId, const char* json) { engine.onInput(wsId, json, g_millis); }
void ha_input_at(uint32_t wsId, const char* json, uint32_t now) {
    g_millis = now;
    engine.onInput(wsId, json, g_millis);
}
void ha_disconnect(uint32_t wsId) { engine.onWsDisconnect(wsId, g_millis); }
int ha_time_reached(uint32_t now, uint32_t deadline) { return haTimeReached(now, deadline); }
uint32_t ha_time_remaining(uint32_t now, uint32_t deadline) {
    return haTimeRemaining(now, deadline);
}
void ha_select_game(int id) { engine.selectGame((uint8_t)id); }
void ha_trivia_clear() { engine.triviaTopicsClear(); }
void ha_trivia_add_topic(const char* name) { engine.triviaAddTopic(name); }
void ha_trivia_add_q(const char* json) { engine.triviaAddQ(json); }
void ha_content_clear() { engine.contentClear(); }
void ha_content_pack(int game, const char* name) { engine.contentPack((uint8_t)game, name); }
void ha_content_item(const char* json) { engine.contentItem(json); }
void ha_round_end() { engine.roundEnd(); }
void ha_reset_scores() { engine.resetScores(); }
void ha_set_lang(const char* l) { engine.setLang(l); }

// Test-only chess hooks (HA_CHESS_TEST), for positions the opening moves can't reach
// quickly and for perft ground truth against the real move generator.
void ha_chess_load(const char* board64, int stm, int rights, int ep, int halfmove,
                    uint32_t wms, uint32_t bms) {
    engine.chessTestLoad(board64, stm, rights, ep, halfmove, wms, bms);
}
uint32_t ha_chess_perft(const char* board64, int stm, int rights, int ep, int depth) {
    return Engine::chessTestPerft(board64, stm, rights, ep, depth);
}

const char* ha_drain() {
    g_drained = "[";
    for(size_t i = 0; i < g_outbox.size(); i++) {
        if(i) g_drained += ",";
        g_drained += g_outbox[i];
    }
    g_drained += "]";
    g_outbox.clear();
    return g_drained.c_str();
}
}
