#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pack files are intentionally bounded before they are parsed or streamed. Callers
// read one byte past this limit, then reject the file, so an oversized pack can never
// be mistaken for a complete valid prefix.
#define HA_PACK_FILE_MAX (16384U)

static inline bool haContentFileSizeAllowed(size_t bytes) {
    return bytes > 0 && bytes <= HA_PACK_FILE_MAX;
}

// Validate the complete raw text file, not a strlen() prefix. Besides rejecting
// embedded NUL, this implements strict UTF-8 (no overlong encodings, surrogates, or
// code points beyond U+10FFFF) so the later C-string parser sees every source byte.
static inline bool haContentFileBytesValid(const uint8_t* data, size_t bytes) {
    if(!data || !haContentFileSizeAllowed(bytes)) return false;
    size_t i = 0;
    while(i < bytes) {
        uint8_t c = data[i];
        if(c == 0) return false;
        if(c < 0x80) {
            i++;
            continue;
        }

        size_t width;
        if(c >= 0xC2 && c <= 0xDF)
            width = 2;
        else if(c >= 0xE0 && c <= 0xEF)
            width = 3;
        else if(c >= 0xF0 && c <= 0xF4)
            width = 4;
        else
            return false;
        if(i + width > bytes) return false;
        for(size_t j = 1; j < width; j++)
            if((data[i + j] & 0xC0) != 0x80) return false;
        if((c == 0xE0 && data[i + 1] < 0xA0) || (c == 0xED && data[i + 1] > 0x9F) ||
           (c == 0xF0 && data[i + 1] < 0x90) || (c == 0xF4 && data[i + 1] > 0x8F))
            return false;
        i += width;
    }
    return true;
}

// Parse an exact correlated content status: "<kind> game=<decimal>". Requiring the
// whole token prevents a delayed/malformed acknowledgement from completing a newer
// transaction. `out_game` is changed only on success.
static inline bool
    haContentStatusGame(const char* token, const char* kind, uint8_t max_game, uint8_t* out_game) {
    if(!token || !kind || !kind[0] || !out_game) return false;
    size_t prefix_len = strlen(kind);
    if(strncmp(token, kind, prefix_len) != 0 || token[prefix_len] != ' ') return false;
    const char* p = token + prefix_len + 1;
    if(strncmp(p, "game=", 5) != 0) return false;
    p += 5;
    if(*p < '0' || *p > '9') return false;

    unsigned value = 0;
    do {
        value = value * 10U + (unsigned)(*p - '0');
        if(value > max_game) return false;
        p++;
    } while(*p >= '0' && *p <= '9');
    if(*p != '\0') return false;

    *out_game = (uint8_t)value;
    return true;
}
