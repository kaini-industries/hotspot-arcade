#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HA_MAX_PLAYERS 3
#define HA_NICK_LEN 20

typedef struct {
    bool used;
    uint8_t pid;
    char nick[HA_NICK_LEN];
    int32_t score;
} HaPlayer;

#include "../../flipper/hotspot-arcade/helpers/ha_roster.h"

int main(void) {
    HaPlayer players[HA_MAX_PLAYERS] = {{0}};

    players[0].score = 999; // stale unused storage must not leak into a fresh seat
    assert(haRosterUpsert(players, 1, "alice"));
    assert(strcmp(players[0].nick, "ALICE") == 0);
    assert(players[0].score == 0);

    players[0].score = 700;
    assert(haRosterUpsert(players, 1, "alice renamed"));
    assert(strcmp(players[0].nick, "ALICE RENAMED") == 0);
    assert(players[0].score == 700); // resume/takeover/profile upsert preserves it

    players[0].used = false; // LEAVE, followed by numeric pid reuse
    assert(haRosterUpsert(players, 1, "new identity"));
    assert(strcmp(players[0].nick, "NEW IDENTITY") == 0);
    assert(players[0].score == 0);
    return 0;
}
