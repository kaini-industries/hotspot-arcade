// Hotspot Arcade firmware for the ESP32-S2 WiFi dev board.
//
// Hosts an open WiFi AP + captive portal that serves a multiplayer game web app
// (streamed from the Flipper into a LittleFS flash partition, then served from flash),
// and acts as the real-time referee over a WebSocket while the Flipper drives the
// session over UART v2. See docs/PROTOCOL.md.
//
// For education/fun on your own hardware. It runs an OPEN access point and a
// catch-all captive page; only operate it where that is allowed.

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>

#include "ha_proto.h"
#include "ha_json.h"
#include "ha_assets.h"
#include "ha_adapter_config.h"
#include "ha_games.h"

#define WS_MSG_MAX 512
#define AP_MAX_CONN 8

static DNSServer dnsServer;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static IPAddress apIP(192, 168, 4, 1);
static char apName[33] = "Hotspot Arcade";
static char joinCode[7] = ""; // optional here; the Cardputer host always configures one
static char knownIdentities[32][HA_IDENTITY_LEN + 1] = {};
static uint8_t knownIdentityCount = 0;
static bool portalRunning = false;
static bool networkSuspendPending = false;
static uint32_t networkSuspendAt = 0;
static char previousApName[33] = "Hotspot Arcade";
static char pendingApName[33] = "";
static uint8_t apMaxConn = AP_MAX_CONN;
static bool fsReady = false; // LittleFS mounted (bundle store)

static AssetStore assets;
static Engine engine;

// Serial (to the Flipper) is written from the loop task and the async web/WS
// task; serialize whole frames so bytes can't interleave. Engine state is also
// touched from both tasks, so guard it too.
static SemaphoreHandle_t serialMutex = nullptr;
static SemaphoreHandle_t engineMutex = nullptr;
#define ENGINE_LOCK() xSemaphoreTakeRecursive(engineMutex, portMAX_DELAY)
#define ENGINE_UNLOCK() xSemaphoreGiveRecursive(engineMutex)

// ---------------- device identity (one phone = one player) ----------------
//
// The engine keys a player on the phone rather than on the socket (see Player in
// ha_games.h), so that a second browser context -- iOS opens the portal in a captive
// mini-browser alongside Safari -- joins the player that phone already has instead of
// becoming a new one. The WebSocket layer only knows a peer IP, and that IP is
// something our own DHCP server made up, so resolve it to the station's MAC (the
// actual device identity) and hand the engine an opaque key built from that.
//
// Phones default to a randomized "private" MAC, but it is stable per SSID: it survives
// reconnects and only changes if the AP is renamed. Exactly the lifetime we need.
//
// The IP -> MAC mapping comes from the AP's own DHCP server: ip_event_ap_staipassigned_t
// carries the assigned address *and* the client MAC, so one event handler can keep a
// small table (the AP caps stations well below HA_STA_MAX). A station that got its lease
// before the handler was installed is missing from it; for those, read the MAC out of
// lwIP's ARP cache instead, and if even that misses, key on the IP.

#define HA_STA_MAX 10 // >= ESP_WIFI_MAX_CONN_NUM: the AP cannot hold more leases
#define HA_KEY_MAC 0x01 // key tag: low 48 bits are a station MAC
#define HA_KEY_IP 0x02 // key tag: low 32 bits are an IPv4 address

struct StaLease {
    uint32_t ip; // 0 = free slot
    uint8_t mac[6];
};
static StaLease staLeases[HA_STA_MAX];
// Written from the WiFi event task, read from the async WS task; the critical section
// is a handful of instructions over a 10-entry array.
static portMUX_TYPE leaseMux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t macDeviceKey(const uint8_t* mac) {
    uint64_t v = 0;
    for(int i = 0; i < 6; i++) v = (v << 8) | mac[i];
    return ((uint64_t)HA_KEY_MAC << 56) | v;
}

static void leaseNote(uint32_t ip, const uint8_t* mac) {
    portENTER_CRITICAL(&leaseMux);
    int slot = -1, freeSlot = -1;
    for(int i = 0; i < HA_STA_MAX; i++) {
        if(!staLeases[i].ip) {
            if(freeSlot < 0) freeSlot = i;
        } else if(memcmp(staLeases[i].mac, mac, 6) == 0) {
            slot = i; // same phone, (re)leased
            break;
        } else if(staLeases[i].ip == ip) {
            slot = i; // this address now belongs to a different station
        }
    }
    if(slot < 0) slot = freeSlot >= 0 ? freeSlot : 0;
    staLeases[slot].ip = ip;
    memcpy(staLeases[slot].mac, mac, 6);
    portEXIT_CRITICAL(&leaseMux);
}

static void leaseForget(const uint8_t* mac) {
    portENTER_CRITICAL(&leaseMux);
    for(int i = 0; i < HA_STA_MAX; i++)
        if(staLeases[i].ip && memcmp(staLeases[i].mac, mac, 6) == 0) staLeases[i].ip = 0;
    portEXIT_CRITICAL(&leaseMux);
}

static void leasesClear() {
    portENTER_CRITICAL(&leaseMux);
    memset(staLeases, 0, sizeof(staLeases));
    portEXIT_CRITICAL(&leaseMux);
}

static bool leaseMac(uint32_t ip, uint8_t* out) {
    bool found = false;
    portENTER_CRITICAL(&leaseMux);
    for(int i = 0; i < HA_STA_MAX && !found; i++)
        if(staLeases[i].ip == ip) {
            memcpy(out, staLeases[i].mac, 6);
            found = true;
        }
    portEXIT_CRITICAL(&leaseMux);
    return found;
}

// The address a MAC currently holds, for the log line only (0 if unknown).
static uint32_t leaseIp(const uint8_t* mac) {
    uint32_t ip = 0;
    portENTER_CRITICAL(&leaseMux);
    for(int i = 0; i < HA_STA_MAX && !ip; i++)
        if(staLeases[i].ip && memcmp(staLeases[i].mac, mac, 6) == 0) ip = staLeases[i].ip;
    portEXIT_CRITICAL(&leaseMux);
    return ip;
}

// Fallback for a station whose lease we never saw. Read without the lwIP core lock:
// the ARP table is a fixed static array, so the worst case is reading a half-updated
// entry (a wrong MAC, i.e. one extra player) rather than a bad pointer.
static bool arpMac(uint32_t ip, uint8_t* out) {
    for(size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t* eip = nullptr;
        struct netif* nif = nullptr;
        struct eth_addr* eth = nullptr;
        if(etharp_get_entry(i, &eip, &nif, &eth) && eip && eth && eip->addr == ip) {
            memcpy(out, eth->addr, 6);
            return true;
        }
    }
    return false;
}

// Which phone a socket is on, as the opaque key the engine stores. 0 = unknown, which
// makes the engine fall back to one player per connection rather than merging clients.
static uint64_t peerDeviceKey(AsyncWebSocketClient* client) {
    uint32_t ip = (uint32_t)client->remoteIP();
    if(!ip || ip == (uint32_t)apIP) return 0; // not a joined station
    uint8_t mac[6];
    if(leaseMac(ip, mac) || arpMac(ip, mac)) return macDeviceKey(mac);
    return ((uint64_t)HA_KEY_IP << 56) | ip; // last resort: the address itself
}

// Render a key for the serial log: "ip=.. mac=.." when we know both.
static String deviceKeyText(uint64_t key) {
    char b[64];
    if((uint8_t)(key >> 56) == HA_KEY_MAC) {
        uint8_t mac[6];
        for(int i = 0; i < 6; i++) mac[i] = (uint8_t)(key >> (40 - 8 * i));
        uint32_t ip = leaseIp(mac);
        String where = ip ? String("ip=") + IPAddress(ip).toString() + " " : String("");
        snprintf(b, sizeof(b), "mac=%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        return where + b;
    }
    if((uint8_t)(key >> 56) == HA_KEY_IP)
        return String("ip=") + IPAddress((uint32_t)(key & 0xFFFFFFFFu)).toString() + " mac=?";
    return String("device=unknown");
}

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

static void uartContentStatus(const char* kind, uint8_t game) {
    char token[40];
    snprintf(token, sizeof(token), "%s game=%u", kind, (unsigned)game);
    uartStatus(token);
}

static void uartTransportState() {
    uint8_t reason;
    uint16_t expected, online;
    uint32_t reconnectMs;
    bool paused, portalLive, suspendPending;
    ENGINE_LOCK();
    paused = engine.transportPaused();
    reason = engine.transportReason();
    expected = engine.transportExpectedMask();
    online = engine.transportOnlineExpectedMask();
    reconnectMs = engine.transportReconnectMs();
    portalLive = portalRunning;
    suspendPending = networkSuspendPending;
    ENGINE_UNLOCK();
    // A portal scheduled to go down is not resume-ready even though the sockets are
    // deliberately left alive for the 200 ms server_pause flush window. Reporting it
    // ready here would let the host immediately undo the pause before shutdown.
    bool networkReady = portalLive && !suspendPending;
    uint8_t p[10] = {
        (uint8_t)((paused ? 0x01 : 0) | (networkReady ? 0x02 : 0) |
                  (expected == online ? 0x04 : 0) | (portalLive ? 0x08 : 0)),
        reason,
        (uint8_t)(expected & 0xFF),
        (uint8_t)(expected >> 8),
        (uint8_t)(online & 0xFF),
        (uint8_t)(online >> 8),
        (uint8_t)(reconnectMs & 0xFF),
        (uint8_t)((reconnectMs >> 8) & 0xFF),
        (uint8_t)((reconnectMs >> 16) & 0xFF),
        (uint8_t)((reconnectMs >> 24) & 0xFF),
    };
    uartSend(HA_MSG_TRANSPORT_STATE, p, sizeof(p));
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
void* haContentAlloc(size_t bytes) {
    void* memory = ps_malloc(bytes);
    return memory ? memory : malloc(bytes);
}
void haContentFree(void* memory) {
    free(memory);
}
bool haContentAllocationAllowed() {
    return true;
}
bool haPhoneGameChangeAllowed(uint8_t fromGame, uint8_t toGame) {
    (void)fromGame;
    (void)toGame;
    return false; // no loop-task/SD request queue exists in this adapter yet
}
static bool identityKnown(const char* identity) {
    if(!identity || strlen(identity) != HA_IDENTITY_LEN) return false;
    for(uint8_t i = 0; i < knownIdentityCount; i++)
        if(strcmp(knownIdentities[i], identity) == 0) return true;
    return false;
}
static void rememberIdentity(const char* identity) {
    if(identityKnown(identity) || knownIdentityCount >= 32) return;
    strlcpy(knownIdentities[knownIdentityCount++], identity, sizeof(knownIdentities[0]));
}
static void clearKnownIdentities() {
    memset(knownIdentities, 0, sizeof(knownIdentities));
    knownIdentityCount = 0;
}
uint8_t haAuthorizeIdentity(
    uint32_t wsId, const char* identity, const char* code, uint32_t* retryMs) {
    (void)wsId;
    if(retryMs) *retryMs = 0;
    if(identityKnown(identity)) return HA_JOIN_AUTH_KNOWN;
    if(!joinCode[0]) return HA_JOIN_AUTH_OK; // legacy Flipper host: open admission
    if(!code || !code[0]) return HA_JOIN_AUTH_REQUIRED;
    return strcmp(code, joinCode) == 0 ? HA_JOIN_AUTH_OK : HA_JOIN_AUTH_BAD_CODE;
}
void haUartJoin(uint8_t pid, const char* nick) {
    uint8_t buf[1 + HA_NICK_LEN + 1];
    buf[0] = pid;
    size_t n = strlen(nick);
    if(n > HA_NICK_LEN) n = HA_NICK_LEN;
    memcpy(buf + 1, nick, n);
    uartSend(HA_MSG_JOIN, buf, 1 + n);
}
void haUartJoinStable(uint8_t pid, const char* identity, const char* nick, const char* avatar) {
    (void)avatar;
    rememberIdentity(identity);
    // The generic Flipper UI still consumes the legacy pid+nickname payload. The
    // stable digest is deliberately kept in the host adapter; richer hosts (such
    // as Cardputer) implement this sink and persist the digest themselves.
    haUartJoin(pid, nick);
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
void haUartHostEvent(
    uint8_t kind,
    uint8_t game,
    uint8_t actor,
    uint8_t target,
    int16_t value,
    const char* text) {
    uint8_t buf[HA_HOST_EVENT_HEADER_SIZE + HA_HOST_EVENT_TEXT_MAX];
    buf[0] = HA_HOST_EVENT_VERSION;
    buf[1] = kind;
    buf[2] = game;
    buf[3] = actor;
    buf[4] = target;
    buf[5] = (uint8_t)((uint16_t)value & 0xFF);
    buf[6] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
    size_t n = text ? strnlen(text, HA_HOST_EVENT_TEXT_MAX) : 0;
    if(n) memcpy(buf + HA_HOST_EVENT_HEADER_SIZE, text, n);
    uartSend(HA_MSG_EVENT, buf, HA_HOST_EVENT_HEADER_SIZE + n);
}
// Finished artwork (Frankendraw). One frame per call -- a sheet header, a single line
// segment, or the end marker -- so a whole drawing streams to the Flipper a segment at
// a time and neither side ever buffers one. Payload is the op byte then the JSON.
void haUartArt(uint8_t op, const String& json) {
    uint8_t buf[192];
    buf[0] = op;
    size_t n = json.length();
    if(n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf + 1, json.c_str(), n);
    uartSend(HA_MSG_ART, buf, 1 + n);
}

// Debug trace of the engine's identity decisions. Serial is the Flipper link, so a
// log line shares the wire with the framed protocol: safe because the receiver only
// starts a frame on the SYNC byte (0xA5) and resyncs on a bad CRC, and because
// haLog() emits printable ASCII only -- a UTF-8 nickname could otherwise carry a
// literal 0xA5 and be mistaken for the start of a frame. Written under the same
// mutex as a frame, so it can never land inside one.
static void haLog(const String& line) {
    if(serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.print("\n[ha] ");
    for(const char* p = line.c_str(); *p; p++)
        Serial.write((*p >= 0x20 && (uint8_t)*p < 0x7F) ? *p : '?');
    Serial.print("\n");
    if(serialMutex) xSemaphoreGive(serialMutex);
}
void haLogJoin(uint8_t pid, uint64_t deviceKey, const char* nick, bool consolidated) {
    String where = deviceKeyText(deviceKey);
    if(consolidated)
        haLog(String("SAME DEVICE ") + where + " -> pid=" + pid + " nick=\"" + nick +
              "\" (consolidated)");
    else
        haLog(String("JOIN pid=") + pid + " " + where + " nick=\"" + nick + "\"");
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
        if(!a) a = assets.root(); // captive-detection URLs and "/" -> the app
        // Serve the ~47 KB app to real browsers -- including the iOS captive WINDOW, which is
        // a WebKit view with a "Mozilla" User-Agent, so the app renders there too. The OS
        // captive-detection *probes* (User-Agent "CaptiveNetworkSupport...") fire several at
        // once; serving the bundle from flash to each copies into per-connection TCP buffers
        // and storms the heap, so those get a tiny landing instead.
        bool browser = request->header("User-Agent").indexOf("Mozilla") >= 0;
        // HTTP caching: the bundle CRC is a strong ETag. A client that already holds this exact
        // bundle revalidates with If-None-Match and gets a tiny 304 -- no 47 KB body -- so a
        // reload or an iOS captive re-probe costs nothing instead of re-sending the whole
        // bundle. The ETag changes when the host updates the bundle, so updates are never stale.
        char etag[12] = "";
        if(browser && fsReady && a && a->len) {
            snprintf(etag, sizeof(etag), "\"%08lx\"", (unsigned long)assets.bundleCrc());
            if(request->header("If-None-Match") == etag) {
                AsyncWebServerResponse* nm = request->beginResponse(304);
                nm->addHeader("ETag", etag);
                request->send(nm);
                return;
            }
        }
        File f;
        if(browser && fsReady && a && a->len) f = LittleFS.open(a->fsname, "r");
        if(!f) {
            request->send(200, "text/html",
                          "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                          "<body style='margin:0;background:#0b0b12;color:#eee;text-align:center;"
                          "font:16px system-ui,sans-serif;padding:2.5em 1em'><h2>Hotspot Arcade</h2>"
                          "<p>Open <b>192.168.4.1</b> in your browser to play.</p></body>");
            return;
        }
        // Streamed from flash (no RAM copy). The stored name ends ".gz" while a->path does
        // not, so AsyncFileResponse sets Content-Encoding: gzip for us; mime is explicit.
        AsyncWebServerResponse* res = request->beginResponse(f, String(a->path), (const char*)a->mime);
        res->addHeader("ETag", etag);
        res->addHeader("Cache-Control", "no-cache");
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
        bool reportTransport;
        ENGINE_LOCK();
        engine.onWsDisconnect(client->id(), millis());
        reportTransport = engine.transportPaused();
        ENGINE_UNLOCK();
        if(reportTransport) uartTransportState();
    } else if(type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT &&
           len < WS_MSG_MAX) {
            char buf[WS_MSG_MAX];
            memcpy(buf, data, len);
            buf[len] = '\0';
            bool accepted = false;
            bool reportTransport = false;
            ENGINE_LOCK();
            // Check lifecycle under the same mutex as onInput and the PAUSE write.
            // A callback that parsed before shutdown but waited for this lock cannot
            // reattach its old socket after detachTransportSockets() has run.
            if(!networkSuspendPending && portalRunning) {
                engine.onInput(client->id(), buf, millis());
                reportTransport = engine.transportPaused();
                accepted = true;
            }
            ENGINE_UNLOCK();
            if(accepted && reportTransport) uartTransportState();
        }
    }
}

// ---------------- AP lifecycle ----------------

// The AP's DHCP server is where a station's IP and MAC are seen together. Registered
// once in setup(), before any AP comes up, so no lease is missed (see peerDeviceKey).
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if(event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        // esp-idf 5+ (Arduino core 3.x, e.g. the C5): the assign event carries the MAC.
        leaseNote(info.wifi_ap_staipassigned.ip.addr, info.wifi_ap_staipassigned.mac);
#else
        // esp-idf 4.x (Arduino core 2.0.x, the S2/WROOM): the staipassigned event has no
        // MAC field, so we can't record the lease here. peerDeviceKey() then resolves the
        // station from the lwIP ARP cache at hello() time, which by then holds its entry.
        (void)info;
#endif
    else if(event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED)
        leaseForget(info.wifi_ap_stadisconnected.mac);
}

static bool startPortal() {
    if(portalRunning) {
        uartStatus("ap_already");
        String up = String("up ip=") + WiFi.softAPIP().toString();
        uartStatus(up.c_str());
        uartTransportState();
        return true;
    }
    leasesClear(); // a fresh session leases fresh addresses
    WiFi.mode(WIFI_AP);
    if(!WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0)) ||
       !WiFi.softAP(apName, nullptr, 1, 0, apMaxConn)) {
        WiFi.softAPdisconnect(true);
        uartStatus("ap_error");
        return false;
    }
    delay(100);
    uartStatus("ap_ok");

    if(!dnsServer.start(53, "*", apIP)) {
        WiFi.softAPdisconnect(true);
        uartStatus("dns_error");
        return false;
    }
    // Publish the lifecycle bit under the mutex used by onWsEvent, before the listener
    // becomes reachable, so a racing first hello is accepted atomically.
    ENGINE_LOCK();
    portalRunning = true;
    ENGINE_UNLOCK();
    server.begin();

    String up = String("up ip=") + WiFi.softAPIP().toString();
    uartStatus(up.c_str());
    uartTransportState();
    return true;
}

static void suspendPortalNetwork() {
    if(portalRunning) {
        // closeAll() only queues the WebSocket close handshake.  Detach the engine's
        // live socket ids first, while its planned expected-mask remains intact, so
        // startPortal() can never publish a stale all-returned transport snapshot.
        ENGINE_LOCK();
        engine.detachTransportSockets(millis());
        portalRunning = false;
        ENGINE_UNLOCK();
        ws.closeAll();
        server.end();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
    }
    leasesClear();
    uartStatus("network_suspended");
    uartTransportState();
}

static void stopPortal() {
    suspendPortalNetwork();
    // Admission memory is session-scoped. A player known to the old party must
    // present the next party's join code after a host stop/new-session cycle.
    ENGINE_LOCK();
    clearKnownIdentities();
    joinCode[0] = '\0';
    engine.reset();
    ENGINE_UNLOCK();
    uartStatus("stopped");
}

// ---------------- UART RX (framed) ----------------

enum RxState { RX_SYNC, RX_TYPE, RX_LEN0, RX_LEN1, RX_PAYLOAD, RX_CRC, RX_RAW };
static RxState rxState = RX_SYNC;
static uint8_t rxType = 0;
static uint16_t rxLen = 0, rxIdx = 0;
static uint8_t rxCrc = 0;
static uint8_t rxBuf[HA_MAX_PAYLOAD + 1];
// Target of the current serialized host content transaction. It survives a failed
// BEGIN/staging bank so the one terminal COMMIT result still carries the requested id.
static uint8_t contentRequestGame = 0xFF;

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
        assets.finishStream(); // file phase done: persist the bundle + its CRC if it changed
        uartStatus("ap_set");
        break;
    case HA_MSG_START:
        if(!startPortal() && pendingApName[0]) {
            strlcpy(apName, previousApName, sizeof(apName));
            pendingApName[0] = '\0';
            // The first pause notice correctly named the requested SSID. From this
            // point onward the proven fallback is authoritative, so resumed phones
            // must not be sent back toward the AP that just failed.
            ENGINE_LOCK();
            (void)engine.replacePausedTransportSsid(apName);
            ENGINE_UNLOCK();
            uartStatus("ap_fallback");
            (void)startPortal();
        }
        if(portalRunning) pendingApName[0] = '\0';
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
        // v21 selection is inseparable from its content transaction. Silently
        // selecting here could expose a zero-pack lobby, so old hosts get an
        // explicit failure and leave the live game untouched.
        uartStatus("select_deprecated");
        break;
    case HA_MSG_CONTENT_BEGIN: {
        bool ok = false;
        uint8_t target = rxLen ? rxBuf[0] : 0xFF;
        contentRequestGame = target;
        if(rxLen >= 1 && rxLen <= 8) {
            char lang[8];
            size_t n = rxLen - 1;
            memcpy(lang, rxBuf + 1, n);
            lang[n] = '\0';
            ENGINE_LOCK();
            ok = engine.contentBegin(rxBuf[0], lang);
            ENGINE_UNLOCK();
        } else {
            // Any BEGIN frame supersedes staging. A malformed one must not leave
            // an older transaction available to an unrelated later COMMIT.
            ENGINE_LOCK();
            engine.contentAbort();
            ENGINE_UNLOCK();
        }
        // BEGIN/PACK/ITEM diagnostics are deliberately nonterminal. Only COMMIT
        // emits content_ok/content_error, so a delayed intermediate failure cannot
        // cancel a later same-game retry on the host.
        uartContentStatus(ok ? "content_begin" : "content_invalid", target);
        break;
    }
    case HA_MSG_CONTENT_PACK:
        // payload: game byte, then the pack name (not NUL-terminated on the wire).
        if(rxLen >= 2 && rxLen <= 64) {
            char name[64];
            size_t n = rxLen - 1;
            memcpy(name, rxBuf + 1, n);
            name[n] = '\0';
            bool ok;
            ENGINE_LOCK();
            ok = engine.contentPack(rxBuf[0], name);
            ENGINE_UNLOCK();
            if(!ok) uartContentStatus("content_invalid", contentRequestGame);
        } else {
            // Poison an existing stage as well as reporting the malformed frame;
            // truncating an overlong name could otherwise commit different content
            // than the host counted and displayed.
            ENGINE_LOCK();
            engine.contentPack(rxLen ? rxBuf[0] : 0, "");
            ENGINE_UNLOCK();
            uartContentStatus("content_invalid", contentRequestGame);
        }
        break;
    case HA_MSG_CONTENT_ITEM: {
        bool ok;
        ENGINE_LOCK();
        ok = engine.contentItem((const char*)rxBuf);
        ENGINE_UNLOCK();
        if(!ok) uartContentStatus("content_invalid", contentRequestGame);
        break;
    }
    case HA_MSG_CONTENT_COMMIT: {
        bool ok = false;
        uint8_t target = contentRequestGame;
        if(rxLen == 4) {
            uint16_t packs = (uint16_t)rxBuf[0] | ((uint16_t)rxBuf[1] << 8);
            uint16_t items = (uint16_t)rxBuf[2] | ((uint16_t)rxBuf[3] << 8);
            ENGINE_LOCK();
            ok = engine.contentCommit(packs, items, millis());
            ENGINE_UNLOCK();
        } else {
            // A malformed commit terminates the transaction; retaining its stage
            // could let an unrelated later COMMIT publish stale content.
            ENGINE_LOCK();
            engine.contentAbort();
            ENGINE_UNLOCK();
        }
        if(ok) {
            // Include the committed id so delayed/back-to-back host requests cannot
            // misassociate an acknowledgement with a newer pending selection.
            uartContentStatus("content_ok", target);
        } else {
            uartContentStatus("content_error", target);
        }
        contentRequestGame = 0xFF;
        break;
    }
    case HA_MSG_CONTENT_ABORT:
        ENGINE_LOCK();
        engine.contentAbort();
        ENGINE_UNLOCK();
        contentRequestGame = 0xFF;
        uartStatus("content_abort");
        break;
    case HA_MSG_TRANSPORT_PAUSE: {
        HaTransportResult result = HA_TRANSPORT_BAD_ARGUMENT;
        if(ha_json_flat_object_valid((const char*)rxBuf)) {
            char reasonText[16] = "", ssid[33] = "";
            int reconnect = -1;
            bool haveReason = ha_json_str((const char*)rxBuf, "reason", reasonText, sizeof(reasonText));
            const char* ssidValue = ha_json_find((const char*)rxBuf, "ssid");
            bool haveSsid = !ssidValue || ha_json_str((const char*)rxBuf, "ssid", ssid, sizeof(ssid));
            bool haveReconnect = ha_json_int((const char*)rxBuf, "reconnect_ms", &reconnect);
            HaTransportReason reason = strcmp(reasonText, "ssid_change") == 0
                                           ? HA_TRANSPORT_SSID_CHANGE
                                           : HA_TRANSPORT_AP_OFF;
            bool validReason = haveReason &&
                               (strcmp(reasonText, "ssid_change") == 0 ||
                                strcmp(reasonText, "ap_off") == 0);
            if(validReason && haveSsid && haveReconnect && reconnect >= 0 && reconnect <= 600000 &&
               (reason != HA_TRANSPORT_SSID_CHANGE || ssid[0])) {
                ENGINE_LOCK();
                result = engine.pauseTransport(reason, ssid, (uint32_t)reconnect, millis());
                if(result == HA_TRANSPORT_OK) {
                    // Publish non-ready before releasing the same mutex used by WS
                    // callbacks. A ping racing this command can therefore never emit
                    // a paused-but-ready snapshot in the gap before shutdown is armed.
                    networkSuspendPending = portalRunning;
                    networkSuspendAt = millis() + 200;
                }
                ENGINE_UNLOCK();
                if(result == HA_TRANSPORT_OK) {
                    strlcpy(previousApName, apName, sizeof(previousApName));
                    if(reason == HA_TRANSPORT_SSID_CHANGE) {
                        strlcpy(pendingApName, ssid, sizeof(pendingApName));
                        strlcpy(apName, ssid, sizeof(apName));
                    } else {
                        pendingApName[0] = '\0';
                    }
                }
            }
        }
        uartStatus(result == HA_TRANSPORT_OK ? "transport_pausing" :
                   result == HA_TRANSPORT_ALREADY ? "transport_already" :
                   result == HA_TRANSPORT_CONFLICT ? "transport_conflict" :
                                                     "transport_error");
        uartTransportState();
        break;
    }
    case HA_MSG_TRANSPORT_RESUME: {
        HaTransportResult result = HA_TRANSPORT_CONFLICT;
        bool validPayload = rxLen == 0 ||
                            (rxLen == 1 &&
                             (rxBuf[0] & ~HA_TRANSPORT_RESUME_EXPIRE_MISSING) == 0);
        bool expireMissing = rxLen == 1 &&
                             (rxBuf[0] & HA_TRANSPORT_RESUME_EXPIRE_MISSING) != 0;
        // Do not accept resume during the pre-shutdown flush window. The host must wait
        // for a genuine post-restart transport snapshot with network-ready set.
        if(validPayload && portalRunning && !networkSuspendPending) {
            ENGINE_LOCK();
            result = engine.resumeTransport(millis(), expireMissing);
            ENGINE_UNLOCK();
        }
        uartStatus(result == HA_TRANSPORT_OK ? "transport_resumed" :
                   result == HA_TRANSPORT_NOT_PAUSED ? "transport_not_paused" :
                                                      "transport_network_down");
        uartTransportState();
        break;
    }
    case HA_MSG_ROUND_END:
        ENGINE_LOCK();
        engine.roundEnd(millis());
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
        if(codeValue && !haParseJoinCodeExact(configJson, code)) {
            uartStatus("config_error");
            break;
        }
        int v;
        if(ha_json_int(configJson, "max", &v) && v >= 1 && v <= HA_MAX_PLAYERS)
            apMaxConn = (uint8_t)v;
        // The async WebSocket path reads admission state only while holding the
        // engine mutex. Update admission state in that same synchronization
        // domain so a hello cannot race a partial CONFIG transition. Locale is
        // committed transactionally with the selected content bank and CONFIG
        // cannot change it ahead of a content commit that may fail.
        ENGINE_LOCK();
        if(codeValue) strlcpy(joinCode, code, sizeof(joinCode));
        ENGINE_UNLOCK();
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
    WiFi.onEvent(onWiFiEvent, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
    WiFi.onEvent(onWiFiEvent, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.addHandler(new ArcadeHandler()).setFilter(ON_AP_FILTER);
    Serial.setRxBufferSize(4096);
    Serial.begin(HA_UART_BAUD);
    delay(100);
    // Mount the bundle store (formats the spiffs-labelled partition as LittleFS on first
    // boot) and load any bundle a previous session persisted. maxOpenFiles is raised
    // because AsyncFileResponse holds a File open per in-flight response.
    fsReady = LittleFS.begin(true, "/littlefs", 16);
    if(fsReady) assets.load();
    engine.reset();
    uartStatus("boot");
}

void loop() {
    if(portalRunning) {
        dnsServer.processNextRequest();
        ws.cleanupClients();
    }
    ENGINE_LOCK();
    engine.tick(millis());
    ENGINE_UNLOCK();
    if(networkSuspendPending && (int32_t)(millis() - networkSuspendAt) >= 0) {
        // Keep pending asserted while closeAll invokes disconnect callbacks; their
        // transport snapshots must not briefly advertise resume-ready mid-shutdown.
        suspendPortalNetwork();
        ENGINE_LOCK();
        networkSuspendPending = false;
        ENGINE_UNLOCK();
    }
    pumpSerial();

    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if(now - lastPing >= 2000) {
        lastPing = now;
        // Identity beacon: magic + version + the CRC of the web bundle we hold in flash +
        // the current game id. Version flags an outdated board; the CRC lets the Flipper skip
        // re-streaming an unchanged bundle; the game id lets it recover the last host-committed
        // selection reliably. Bytes 6-10 are new in v19; bytes 11-14 (free heap KB, free PSRAM KB, LE
        // uint16 each) are new in v1.7.1 for the Flipper's memory readout. Older Flippers read
        // only the bytes they know and ignore the rest, so growing this stays compatible.
        uint32_t bcrc = fsReady ? assets.bundleCrc() : 0;
        uint16_t heapKb = (uint16_t)(ESP.getFreeHeap() / 1024);
        uint16_t psramKb = (uint16_t)(ESP.getFreePsram() / 1024);
        uint8_t beacon[15] = {
            HA_FW_MAGIC_0,
            HA_FW_MAGIC_1,
            HA_FW_MAGIC_2,
            HA_FW_MAGIC_3,
            (uint8_t)(HA_FW_VERSION & 0xFF),
            (uint8_t)(HA_FW_VERSION >> 8),
            (uint8_t)(bcrc & 0xFF),
            (uint8_t)((bcrc >> 8) & 0xFF),
            (uint8_t)((bcrc >> 16) & 0xFF),
            (uint8_t)((bcrc >> 24) & 0xFF),
            engine.activeGame(),
            (uint8_t)(heapKb & 0xFF),
            (uint8_t)(heapKb >> 8),
            (uint8_t)(psramKb & 0xFF),
            (uint8_t)(psramKb >> 8)};
        uartSend(HA_MSG_PING, beacon, sizeof(beacon));
        // Lifecycle STATUS and state frames are individually CRC-protected but not
        // acknowledged. Repeat the authoritative snapshot with the existing beacon
        // so dropping any single `up`, `network_suspended`, or TRANSPORT_STATE frame
        // converges within two seconds (including an empty expected-player mask).
        uartTransportState();
    }
}
