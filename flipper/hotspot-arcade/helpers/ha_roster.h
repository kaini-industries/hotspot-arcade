#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pure roster helpers shared by the Flipper consumer and a native protocol
// regression test. The including translation unit provides HaPlayer plus the
// HA_MAX_PLAYERS / HA_NICK_LEN bounds.
static inline int haRosterFind(HaPlayer* players, uint8_t pid) {
    for(int i = 0; i < HA_MAX_PLAYERS; i++)
        if(players[i].used && players[i].pid == pid) return i;
    return -1;
}

static inline void haRosterUpper(char* s) {
    for(; s && *s; s++)
        if(*s >= 'a' && *s <= 'z') *s -= 32;
}

// JOIN is an idempotent upsert in protocol v21. Existing pids retain their
// mirrored score across resume/takeover/profile edits; a slot made unused by
// LEAVE is a fresh seat and starts at zero when its pid is reused.
static inline bool haRosterUpsert(HaPlayer* players, uint8_t pid, const char* nick) {
    int idx = haRosterFind(players, pid);
    bool fresh = idx < 0;
    if(fresh) {
        for(int i = 0; i < HA_MAX_PLAYERS; i++)
            if(!players[i].used) {
                idx = i;
                break;
            }
    }
    if(idx < 0) return false;

    HaPlayer* p = &players[idx];
    p->used = true;
    p->pid = pid;
    const char* source = (nick && nick[0]) ? nick : "PLAYER";
    size_t len = strlen(source);
    if(len >= HA_NICK_LEN) len = HA_NICK_LEN - 1;
    memcpy(p->nick, source, len);
    p->nick[len] = '\0';
    haRosterUpper(p->nick);
    if(fresh) p->score = 0;
    return true;
}
