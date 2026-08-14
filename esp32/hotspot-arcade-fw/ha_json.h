// Minimal JSON helpers for the tiny WebSocket / UART messages. Dependency-free
// (no ArduinoJson): incoming messages are small and flat, so a shallow scan is
// enough. Not a general JSON parser; good for `{"t":"answer","c":2}`-shaped data.
#pragma once
#include <Arduino.h>
#include <limits.h>

static inline size_t ha_json_utf8_width(const unsigned char* p) {
    unsigned char c = p[0];
    if(c < 0x80) return c >= 0x20 ? 1 : 0;
    if(c >= 0xC2 && c <= 0xDF) {
        if(!p[1]) return 0;
        return (p[1] & 0xC0) == 0x80 ? 2 : 0;
    }
    if(c >= 0xE0 && c <= 0xEF) {
        if(!p[1] || !p[2]) return 0;
        unsigned char c1 = p[1], c2 = p[2];
        if((c2 & 0xC0) != 0x80) return 0;
        if(c == 0xE0) return (c1 >= 0xA0 && c1 <= 0xBF) ? 3 : 0;
        if(c == 0xED) return (c1 >= 0x80 && c1 <= 0x9F) ? 3 : 0;
        return (c1 & 0xC0) == 0x80 ? 3 : 0;
    }
    if(c >= 0xF0 && c <= 0xF4) {
        if(!p[1] || !p[2] || !p[3]) return 0;
        unsigned char c1 = p[1], c2 = p[2], c3 = p[3];
        if((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return 0;
        if(c == 0xF0) return (c1 >= 0x90 && c1 <= 0xBF) ? 4 : 0;
        if(c == 0xF4) return (c1 >= 0x80 && c1 <= 0x8F) ? 4 : 0;
        return (c1 & 0xC0) == 0x80 ? 4 : 0;
    }
    return 0;
}

static inline void ha_json_ws(const char*& p) {
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
}

static inline bool ha_json_skip_string(const char*& p) {
    if(*p++ != '"') return false;
    while(*p && *p != '"') {
        if(*p == '\\') {
            p++;
            if(*p == '"' || *p == '\\' || *p == '/' || *p == 'b' || *p == 'f' ||
               *p == 'n' || *p == 'r' || *p == 't') { p++; continue; }
            return false;
        }
        size_t width = ha_json_utf8_width((const unsigned char*)p);
        if(width == 0) return false;
        p += width;
    }
    if(*p != '"') return false;
    p++;
    return true;
}

static inline bool ha_json_skip_number(const char*& p) {
    if(*p == '-') p++;
    if(*p == '0') {
        p++;
        if(*p >= '0' && *p <= '9') return false;
    } else {
        if(*p < '1' || *p > '9') return false;
        while(*p >= '0' && *p <= '9') p++;
    }
    if(*p == '.') {
        p++;
        if(*p < '0' || *p > '9') return false;
        while(*p >= '0' && *p <= '9') p++;
    }
    if(*p == 'e' || *p == 'E') {
        p++;
        if(*p == '+' || *p == '-') p++;
        if(*p < '0' || *p > '9') return false;
        while(*p >= '0' && *p <= '9') p++;
    }
    return true;
}

// Validate a complete, flat object before field lookup. Duplicate keys are
// intentionally allowed because Spyfall pack records repeat the `r` key.
static inline bool ha_json_flat_object_valid(const char* json) {
    if(!json) return false;
    const char* p = json;
    ha_json_ws(p);
    if(*p++ != '{') return false;
    ha_json_ws(p);
    if(*p == '}') { p++; ha_json_ws(p); return *p == '\0'; }
    for(;;) {
        if(!ha_json_skip_string(p)) return false;
        ha_json_ws(p);
        if(*p++ != ':') return false;
        ha_json_ws(p);
        if(*p == '"') {
            if(!ha_json_skip_string(p)) return false;
        } else if(*p == '-' || (*p >= '0' && *p <= '9')) {
            if(!ha_json_skip_number(p)) return false;
        } else if(strncmp(p, "true", 4) == 0) {
            p += 4;
        } else if(strncmp(p, "false", 5) == 0) {
            p += 5;
        } else if(strncmp(p, "null", 4) == 0) {
            p += 4;
        } else {
            return false;
        }
        ha_json_ws(p);
        if(*p == ',') { p++; ha_json_ws(p); continue; }
        if(*p++ != '}') return false;
        ha_json_ws(p);
        return *p == '\0';
    }
}

// Find `"key"` then the following `:` and return a pointer just past the colon
// (skipping spaces), or nullptr. Only scans the top level well enough for our
// flat objects (values are strings/ints/arrays we control).
// Find the `n`-th (0-based) occurrence of `"key":` and return a pointer just past the
// colon. Repeats matter because the Flipper's pack streamer emits one JSON pair per
// "Key: value" line of a content block, so a block that repeats a key (Spyfall's
// several "R:" role lines) really does arrive as a repeated key in one object.
static inline const char* ha_json_find_nth(const char* s, const char* key, int n) {
    size_t klen = strlen(key);
    for(const char* p = s; *p; p++) {
        if(p[0] != '"') continue;
        if(strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char* q = p + 1 + klen + 1;
            while(*q == ' ') q++;
            if(*q != ':') continue;
            q++;
            while(*q == ' ') q++;
            if(n-- <= 0) return q;
        }
    }
    return nullptr;
}

static inline const char* ha_json_find(const char* s, const char* key) {
    return ha_json_find_nth(s, key, 0);
}

// Read the `nth` (0-based) string value for `key` into out. Same unescaping as
// ha_json_str; used to walk a content block's repeated keys in file order.
static inline bool ha_json_str_nth(const char* s, const char* key, int nth, char* out,
                                   size_t n) {
    if(!out || n == 0) return false;
    out[0] = '\0';
    const char* q = ha_json_find_nth(s, key, nth);
    if(!q || *q != '"') return false;
    q++;
    size_t i = 0;
    while(*q && *q != '"') {
        if(*q == '\\') {
            if(!q[1]) return false;
            q++;
            char c = *q;
            if(c == 'n') c = '\n';
            else if(c == 't') c = '\t';
            else if(c == 'r') c = '\r';
            else if(c == 'b') c = '\b';
            else if(c == 'f') c = '\f';
            else if(c == '"' || c == '\\' || c == '/') {}
            else { out[0] = '\0'; return false; }
            if(i + 1 >= n) { out[0] = '\0'; return false; }
            out[i++] = c;
            q++;
        } else {
            size_t width = ha_json_utf8_width((const unsigned char*)q);
            if(width == 0 || i + width >= n) { out[0] = '\0'; return false; }
            memcpy(out + i, q, width);
            i += width;
            q += width;
        }
    }
    if(*q != '"') { out[0] = '\0'; return false; }
    q++;
    ha_json_ws(q);
    if(*q != ',' && *q != '}' && *q != '\0') { out[0] = '\0'; return false; }
    out[i] = '\0';
    return true;
}

// Read a string value for `key` into out (NUL-terminated, basic \" \\ unescape).
static inline bool ha_json_str(const char* s, const char* key, char* out, size_t n) {
    return ha_json_str_nth(s, key, 0, out, n);
}

// Read an integer value for `key`. Returns false if absent/non-numeric.
static inline bool ha_json_int(const char* s, const char* key, int* out) {
    const char* q = ha_json_find(s, key);
    if(!q) return false;
    bool neg = false;
    if(*q == '-') {
        neg = true;
        q++;
    }
    if(*q < '0' || *q > '9') return false;
    uint32_t v = 0;
    const uint32_t limit = neg ? (uint32_t)INT_MAX + 1U : (uint32_t)INT_MAX;
    while(*q >= '0' && *q <= '9') {
        uint32_t digit = (uint32_t)(*q++ - '0');
        if(v > (limit - digit) / 10U) return false;
        v = v * 10U + digit;
    }
    ha_json_ws(q);
    if(*q != ',' && *q != '}') return false;
    if(neg)
        *out = v == (uint32_t)INT_MAX + 1U ? INT_MIN : -(int)v;
    else
        *out = (int)v;
    return true;
}

// Escape a string for embedding in JSON output.
static inline String ha_json_escape(const char* s) {
    String out;
    out.reserve(strlen(s) + 4);
    for(const char* p = s; *p; p++) {
        char c = *p;
        if(c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if(c == '\n') {
            out += "\\n";
        } else if((unsigned char)c < 0x20) {
            // drop other control chars
        } else {
            out += c;
        }
    }
    return out;
}
