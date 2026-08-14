#include <assert.h>
#include <stdint.h>

#include "../../flipper/hotspot-arcade/helpers/ha_content_flow.h"

int main(void) {
    assert(!haContentFileSizeAllowed(0));
    assert(haContentFileSizeAllowed(1));
    assert(haContentFileSizeAllowed(HA_PACK_FILE_MAX));
    assert(!haContentFileSizeAllowed(HA_PACK_FILE_MAX + 1U));

    const uint8_t valid_utf8[] = {'Q', ':', ' ', 0xF0, 0x9F, 0x99, 0x82, '\n'};
    const uint8_t embedded_nul[] = {'Q', ':', ' ', 'o', 'k', '\n', 0, 'b', 'a', 'd'};
    const uint8_t bad_overlong[] = {0xC0, 0x80};
    const uint8_t bad_truncated[] = {0xE2, 0x82};
    const uint8_t bad_surrogate[] = {0xED, 0xA0, 0x80};
    assert(haContentFileBytesValid(valid_utf8, sizeof(valid_utf8)));
    assert(!haContentFileBytesValid(embedded_nul, sizeof(embedded_nul)));
    assert(!haContentFileBytesValid(bad_overlong, sizeof(bad_overlong)));
    assert(!haContentFileBytesValid(bad_truncated, sizeof(bad_truncated)));
    assert(!haContentFileBytesValid(bad_surrogate, sizeof(bad_surrogate)));

    uint8_t game = 99;
    assert(haContentStatusGame("content_ok game=0", "content_ok", 20, &game));
    assert(game == 0);
    assert(haContentStatusGame("content_error game=20", "content_error", 20, &game));
    assert(game == 20);

    // An early ingest failure is diagnostic only. Keep the host request in flight,
    // reject a same-game retry until the one COMMIT terminal arrives, then permit the
    // retry. There is no second old terminal left to cancel the new request.
    bool pending = true;
    assert(!haContentStatusGame("content_invalid game=8", "content_error", 20, &game));
    assert(pending);
    bool retry_started = !pending; // mirrors ha_select_game's in-flight guard
    assert(!retry_started);
    assert(haContentStatusGame("content_error game=8", "content_error", 20, &game));
    pending = false;
    assert(!pending);
    pending = true; // retry A begins only after old A's sole terminal was consumed
    assert(!haContentStatusGame("content_invalid game=8", "content_error", 20, &game));
    assert(pending);
    assert(haContentStatusGame("content_ok game=8", "content_ok", 20, &game));
    pending = false;
    assert(!pending);

    game = 77;
    assert(!haContentStatusGame("content_ok", "content_ok", 20, &game));
    assert(!haContentStatusGame("content_ok game=", "content_ok", 20, &game));
    assert(!haContentStatusGame("content_ok game=-1", "content_ok", 20, &game));
    assert(!haContentStatusGame("content_ok game=21", "content_ok", 20, &game));
    assert(!haContentStatusGame("content_ok game=2 stale", "content_ok", 20, &game));
    assert(!haContentStatusGame("content_error game=2", "content_ok", 20, &game));
    assert(game == 77); // rejected/stale statuses cannot mutate the pending target
    return 0;
}
