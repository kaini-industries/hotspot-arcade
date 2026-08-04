// Hotspot Arcade firmware for the ESP32-S2 WiFi dev board.
//
// Hosts an open WiFi AP + captive portal that serves a multiplayer game web app
// (streamed from the Flipper into RAM), and acts as the real-time referee over a
// WebSocket while the Flipper drives the session over UART v2. See docs/PROTOCOL.md.
//
// For education/fun on your own hardware. It runs an OPEN access point and a
// catch-all captive page; only operate it where that is allowed.

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>

#include "ha_proto.h"
#include "ha_json.h"
#include "ha_assets.h"
#include "ha_games.h"

#define WS_MSG_MAX 512
#ifndef AP_MAX_CONN
#define AP_MAX_CONN 8
#endif

static DNSServer dnsServer;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static IPAddress apIP(192, 168, 4, 1);
static char apName[33] = "Hotspot Arcade";
static char joinCode[7] = ""; // optional generic-host admission code; Cardputer always sets one
static char knownIdentities[32][HA_IDENTITY_BYTES * 2 + 1] = {};
static uint8_t knownIdentityCount = 0;
static bool portalRunning = false;
static uint8_t apMaxConn = AP_MAX_CONN;

static AssetStore assets;
static Engine engine;

// Serial (to the Flipper) is written from the loop task and the async web/WS
// task; serialize whole frames so bytes can't interleave. Engine state is also
// touched from both tasks, so guard it too.
static SemaphoreHandle_t serialMutex = nullptr;
static SemaphoreHandle_t engineMutex = nullptr;
#define ENGINE_LOCK() xSemaphoreTakeRecursive(engineMutex, portMAX_DELAY)
#define ENGINE_UNLOCK() xSemaphoreGiveRecursive(engineMutex)

// ---------------- UART TX ----------------

static void uartSend(uint8_t type, const uint8_t* payload, size_t len) {
    if(len > HA_MAX_PAYLOAD) len = HA_MAX_PAYLOAD;
    uint8_t hdr[4] = {HA_SYNC, type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    uint8_t crc = ha_crc8_upd(0, type);
    crc = ha_crc8_upd(crc, hdr[2]);
    crc = ha_crc8_upd(crc, hdr[3]);
    for(size_t i = 0; i < len; i++) crc = ha_crc8_upd(crc, payload[i]);

    if(serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.write(hdr, 4);
    if(len) Serial.write(payload, len);
    Serial.write(crc);
    if(serialMutex) xSemaphoreGive(serialMutex);
}

static void uartStatus(const char* token) {
    uartSend(HA_MSG_STATUS, (const uint8_t*)token, strlen(token));
}

// ---------------- sinks used by the engine ----------------

void haWsSendWs(uint32_t wsId, const String& msg) {
    if(!wsId) return;
    ws.text(wsId, msg);
}
void haWsCloseWs(uint32_t wsId) {
    if(wsId) ws.close(wsId, 1008, "identity takeover");
}
void haWsBroadcast(const String& msg) {
    ws.textAll(msg);
}
static bool identityKnown(const char* identity) {
    if(!identity || strlen(identity) != HA_IDENTITY_BYTES * 2) return false;
    for(uint8_t i = 0; i < knownIdentityCount; i++)
        if(strcmp(knownIdentities[i], identity) == 0) return true;
    return false;
}
static void rememberIdentity(const char* identity) {
    if(identityKnown(identity) || knownIdentityCount >= 32) return;
    strlcpy(knownIdentities[knownIdentityCount++], identity, sizeof(knownIdentities[0]));
}
uint8_t haAuthorizeIdentity(
    uint32_t wsId, const char* identity, const char* code, uint32_t* retryMs) {
    (void)wsId;
    if(retryMs) *retryMs = 0;
    if(identityKnown(identity)) return HA_JOIN_AUTH_KNOWN;
    if(!joinCode[0]) return HA_JOIN_AUTH_OK; // legacy Flipper hosts may omit admission control
    if(!code || !code[0]) return HA_JOIN_AUTH_REQUIRED;
    return strcmp(code, joinCode) == 0 ? HA_JOIN_AUTH_OK : HA_JOIN_AUTH_BAD_CODE;
}

static uint8_t haHexNibble(char c) {
    if(c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if(c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0;
}

void haUartJoinStable(uint8_t pid, const char* identity, const char* nick, const char* avatar) {
    (void)avatar;
    rememberIdentity(identity);
    uint8_t buf[1 + 16 + HA_NICK_LEN + 1];
    buf[0] = pid;
    for(size_t i = 0; i < 16; i++)
        buf[1 + i] = (uint8_t)((haHexNibble(identity[i * 2]) << 4) | haHexNibble(identity[i * 2 + 1]));
    size_t n = strlen(nick);
    if(n > HA_NICK_LEN) n = HA_NICK_LEN;
    memcpy(buf + 17, nick, n);
    uartSend(HA_MSG_JOIN, buf, 17 + n);
}
void haUartLeave(uint8_t pid) {
    uartSend(HA_MSG_LEAVE, &pid, 1);
}
void haUartScore(uint8_t pid, int delta, const char* reason) {
    uint8_t buf[1 + 2 + 24];
    buf[0] = pid;
    int16_t d = (int16_t)delta;
    buf[1] = (uint8_t)(d & 0xFF);
    buf[2] = (uint8_t)((d >> 8) & 0xFF);
    size_t n = strlen(reason);
    if(n > 24) n = 24;
    memcpy(buf + 3, reason, n);
    uartSend(HA_MSG_SCORE, buf, 3 + n);
}
void haUartHostEvent(uint8_t kind, uint8_t game, uint8_t actor, uint8_t target,
                     int16_t value, const char* text) {
    uint8_t buf[7 + HA_HOST_EVENT_TEXT_MAX];
    buf[0] = HA_HOST_EVENT_VERSION;
    buf[1] = kind;
    buf[2] = game;
    buf[3] = actor;
    buf[4] = target;
    buf[5] = (uint8_t)(value & 0xFF);
    buf[6] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
    size_t n = text ? strlen(text) : 0;
    if(n > HA_HOST_EVENT_TEXT_MAX) n = HA_HOST_EVENT_TEXT_MAX;
    if(n) memcpy(buf + 7, text, n);
    uartSend(HA_MSG_EVENT, buf, 7 + n);
}

// ---------------- HTTP (captive) ----------------

// Serve the streamed web bundle for every host/path so the captive portal always
// resolves. GET "/" (and every OS captive-probe URL) gets the app; other stored
// asset paths are served by exact match.
class ArcadeHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        (void)request;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* request) override {
        String url = request->url();
        const Asset* a = assets.find(url.c_str());
        if(!a) a = assets.root(); // captive-detection URLs -> the app
        if(!a || !a->buf || a->len == 0) {
            request->send(200, "text/html", "<h1>Hotspot Arcade</h1><p>No bundle loaded.</p>");
            return;
        }
        AsyncWebServerResponse* res =
            request->beginResponse(200, a->mime, (const uint8_t*)a->buf, a->len);
        if(a->gzip) res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        request->send(res);
    }
};

// ---------------- WebSocket ----------------

static void onWsEvent(
    AsyncWebSocket* srv,
    AsyncWebSocketClient* client,
    AwsEventType type,
    void* arg,
    uint8_t* data,
    size_t len) {
    (void)srv;
    if(type == WS_EVT_DISCONNECT) {
        ENGINE_LOCK();
        engine.onWsDisconnect(client->id(), millis());
        ENGINE_UNLOCK();
    } else if(type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT &&
           len < WS_MSG_MAX) {
            char buf[WS_MSG_MAX];
            memcpy(buf, data, len);
            buf[len] = '\0';
            ENGINE_LOCK();
            engine.onInput(client->id(), buf, millis());
            ENGINE_UNLOCK();
        }
    }
}

// ---------------- AP lifecycle ----------------

static void startPortal() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName, nullptr, 1, 0, apMaxConn); // open AP, up to apMaxConn stations
    delay(100);
    uartStatus("ap_ok");

    dnsServer.start(53, "*", apIP);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.addHandler(new ArcadeHandler()).setFilter(ON_AP_FILTER);
    server.begin();
    portalRunning = true;
    ENGINE_LOCK();
    engine.transportResume(millis());
    ENGINE_UNLOCK();

    String up = String("up ip=") + WiFi.softAPIP().toString();
    uartStatus(up.c_str());
}

static void stopPortal() {
    ENGINE_LOCK();
    if(portalRunning) engine.announceServerPause("ap_off", apName, 600000);
    engine.transportPause(millis());
    ENGINE_UNLOCK();
    if(portalRunning) {
        ws.closeAll();
        server.end();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        portalRunning = false;
    }
    uartStatus("stopped");
}

// ---------------- UART RX (framed) ----------------

enum RxState { RX_SYNC, RX_TYPE, RX_LEN0, RX_LEN1, RX_PAYLOAD, RX_CRC, RX_RAW };
static RxState rxState = RX_SYNC;
static uint8_t rxType = 0;
static uint16_t rxLen = 0, rxIdx = 0;
static uint8_t rxCrc = 0;
static uint8_t rxBuf[HA_MAX_PAYLOAD + 1];

// FILE_BEGIN payload: flags(1) pathlen(1) path mimelen(1) mime total(4 LE)
static void handleFileBegin(const uint8_t* p, size_t len) {
    if(len < 3) return;
    size_t i = 0;
    uint8_t flags = p[i++];
    uint8_t pathlen = p[i++];
    if(i + pathlen + 1 > len) return;
    char path[HA_ASSET_PATH];
    size_t pl = pathlen < HA_ASSET_PATH - 1 ? pathlen : HA_ASSET_PATH - 1;
    memcpy(path, p + i, pl);
    path[pl] = '\0';
    i += pathlen;
    uint8_t mimelen = p[i++];
    if(i + mimelen + 4 > len) return;
    char mime[HA_ASSET_MIME];
    size_t ml = mimelen < HA_ASSET_MIME - 1 ? mimelen : HA_ASSET_MIME - 1;
    memcpy(mime, p + i, ml);
    mime[ml] = '\0';
    i += mimelen;
    uint32_t total = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) |
                     ((uint32_t)p[i + 3] << 24);
    assets.begin(path, mime, flags & 1, total);
}

static bool parseJoinCodeExact(const char* json, char out[7]) {
    const char* q = ha_json_find(json, "code");
    if(!q) return false;
    if(*q++ != '"') return false;
    for(size_t i = 0; i < 6; i++) {
        if(q[i] < '0' || q[i] > '9') return false;
        out[i] = q[i];
    }
    if(q[6] != '"') return false;
    const char* tail = q + 7;
    while(*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n') tail++;
    if(*tail != ',' && *tail != '}' && *tail != '\0') return false;
    out[6] = '\0';
    return true;
}

static void dispatchFrame() {
    rxBuf[rxLen] = '\0'; // JSON payloads are text; safe (buf has +1)
    switch(rxType) {
    case HA_MSG_CLEAR_FILES:
        assets.clear();
        uartStatus("cleared");
        break;
    case HA_MSG_FILE_BEGIN:
        handleFileBegin(rxBuf, rxLen);
        if(assets.receiving()) {
            rxState = RX_RAW; // the file bytes follow, unframed
            return;
        }
        uartStatus("fok"); // zero-length file committed immediately
        break;
    case HA_MSG_SET_AP:
        if(rxLen > 0) {
            size_t n = rxLen < sizeof(apName) - 1 ? rxLen : sizeof(apName) - 1;
            memcpy(apName, rxBuf, n);
            apName[n] = '\0';
        }
        uartStatus("ap_set");
        break;
    case HA_MSG_START:
        startPortal();
        break;
    case HA_MSG_STOP:
        stopPortal();
        break;
    case HA_MSG_RESET:
        uartStatus("resetting");
        delay(50);
        ESP.restart();
        break;
    case HA_MSG_SELECT_GAME:
        if(rxLen >= 1) {
            ENGINE_LOCK();
            engine.selectGame(rxBuf[0]);
            ENGINE_UNLOCK();
        }
        break;
    case HA_MSG_CONTENT_CLEAR:
        ENGINE_LOCK();
        engine.contentClear();
        ENGINE_UNLOCK();
        break;
    case HA_MSG_CONTENT_PACK:
        // payload: game byte, then the pack name (not NUL-terminated on the wire).
        if(rxLen >= 2) {
            char name[64];
            size_t n = rxLen - 1 < sizeof(name) - 1 ? (size_t)(rxLen - 1) : sizeof(name) - 1;
            memcpy(name, rxBuf + 1, n);
            name[n] = '\0';
            ENGINE_LOCK();
            engine.contentPack(rxBuf[0], name);
            ENGINE_UNLOCK();
        }
        break;
    case HA_MSG_CONTENT_ITEM:
        ENGINE_LOCK();
        engine.contentItem((const char*)rxBuf);
        ENGINE_UNLOCK();
        break;
    case HA_MSG_CONTENT_COMMIT:
        {
        if(rxLen != 4) {
            ENGINE_LOCK();
            engine.contentAbort();
            ENGINE_UNLOCK();
            uartStatus("content_error");
            break;
        }
        uint16_t expectedPacks = (uint16_t)rxBuf[0] | ((uint16_t)rxBuf[1] << 8);
        uint16_t expectedItems = (uint16_t)rxBuf[2] | ((uint16_t)rxBuf[3] << 8);
        ENGINE_LOCK();
        bool ok = engine.contentCommit(expectedPacks, expectedItems);
        ENGINE_UNLOCK();
        uartStatus(ok ? "content_ok" : "content_error");
        }
        break;
    case HA_MSG_ROUND_END:
        ENGINE_LOCK();
        engine.roundEnd();
        ENGINE_UNLOCK();
        break;
    case HA_MSG_CONFIG: {
        const char* configJson = (const char*)rxBuf;
        if(!ha_json_flat_object_valid(configJson)) {
            uartStatus("config_error");
            break;
        }
        const char* codeValue = ha_json_find(configJson, "code");
        char code[7];
        if(codeValue && !parseJoinCodeExact(configJson, code)) {
            uartStatus("config_error");
            break;
        }
        int v;
        if(ha_json_int(configJson, "max", &v) && v >= 1 && v <= HA_MAX_PLAYERS)
            apMaxConn = (uint8_t)v;
        char lang[8];
        if(ha_json_str(configJson, "lang", lang, sizeof(lang))) {
            ENGINE_LOCK();
            engine.setLang(lang);
            ENGINE_UNLOCK();
        }
        if(codeValue) strlcpy(joinCode, code, sizeof(joinCode));
        break;
    }
    case HA_MSG_RESET_SCORES:
        ENGINE_LOCK();
        engine.resetScores();
        ENGINE_UNLOCK();
        break;
    default:
        break;
    }
    rxState = RX_SYNC;
}

static void rxByte(uint8_t c) {
    switch(rxState) {
    case RX_SYNC:
        if(c == HA_SYNC) rxState = RX_TYPE;
        break;
    case RX_TYPE:
        rxType = c;
        rxCrc = ha_crc8_upd(0, c);
        rxState = RX_LEN0;
        break;
    case RX_LEN0:
        rxLen = c;
        rxCrc = ha_crc8_upd(rxCrc, c);
        rxState = RX_LEN1;
        break;
    case RX_LEN1:
        rxLen |= ((uint16_t)c << 8);
        rxCrc = ha_crc8_upd(rxCrc, c);
        if(rxLen > HA_MAX_PAYLOAD) {
            rxState = RX_SYNC; // bogus length, resync
            break;
        }
        rxIdx = 0;
        rxState = rxLen ? RX_PAYLOAD : RX_CRC;
        break;
    case RX_PAYLOAD:
        rxBuf[rxIdx++] = c;
        rxCrc = ha_crc8_upd(rxCrc, c);
        if(rxIdx >= rxLen) rxState = RX_CRC;
        break;
    case RX_CRC:
        if(c == rxCrc) {
            dispatchFrame(); // sets next state (RX_SYNC or RX_RAW)
        } else {
            rxState = RX_SYNC; // corrupt frame, drop and resync
        }
        break;
    case RX_RAW:
        break; // handled in bulk below
    }
}

static void pumpSerial() {
    while(Serial.available()) {
        if(rxState == RX_RAW) {
            // Bulk asset bytes: drain as many as we still need in one go.
            size_t need = assets.remaining();
            if(need == 0) {
                rxState = RX_SYNC;
                uartStatus("fok");
                continue;
            }
            uint8_t tmp[256];
            size_t want = need < sizeof(tmp) ? need : sizeof(tmp);
            int got = Serial.read(tmp, want);
            if(got > 0) assets.feed(tmp, (size_t)got);
            if(assets.remaining() == 0) {
                rxState = RX_SYNC;
                uartStatus("fok");
            }
        } else {
            rxByte((uint8_t)Serial.read());
        }
    }
}

// ---------------- Arduino entry ----------------

void setup() {
    serialMutex = xSemaphoreCreateMutex();
    engineMutex = xSemaphoreCreateRecursiveMutex();
    Serial.setRxBufferSize(4096);
    Serial.begin(HA_UART_BAUD);
    delay(100);
    engine.reset(millis());
    uartStatus("boot");
}

void loop() {
    if(portalRunning) {
        dnsServer.processNextRequest();
        ws.cleanupClients(apMaxConn);
        ENGINE_LOCK();
        engine.tick(millis());
        ENGINE_UNLOCK();
    }
    pumpSerial();

    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if(now - lastPing >= 2000) {
        lastPing = now;
        // Identity beacon: magic + version, so the Flipper knows it's our firmware
        // (and which version), not a different project on the same board.
        uint8_t beacon[6] = {
            HA_FW_MAGIC_0,
            HA_FW_MAGIC_1,
            HA_FW_MAGIC_2,
            HA_FW_MAGIC_3,
            (uint8_t)(HA_FW_VERSION & 0xFF),
            (uint8_t)(HA_FW_VERSION >> 8)};
        uartSend(HA_MSG_PING, beacon, sizeof(beacon));
    }
}
