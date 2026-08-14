#include <cassert>
#include <cstring>

#include "../../esp32/hotspot-arcade-fw/ha_adapter_config.h"

static void accepts(const char* json, const char* expected) {
    assert(ha_json_flat_object_valid(json));
    char out[7] = "stale";
    assert(haParseJoinCodeExact(json, out));
    assert(std::strcmp(out, expected) == 0);
}

static void rejectsWithoutMutation(const char* json) {
    assert(ha_json_flat_object_valid(json));
    char out[7] = "654321";
    assert(!haParseJoinCodeExact(json, out));
    assert(std::strcmp(out, "654321") == 0);
}

int main() {
    accepts("{\"code\":\"\"}", "");
    accepts("{\"max\":8,\"code\":\"123456\",\"lang\":\"de\"}", "123456");
    rejectsWithoutMutation("{\"code\":\"12345\"}");
    rejectsWithoutMutation("{\"code\":\"1234567\"}");
    rejectsWithoutMutation("{\"code\":\"12x456\"}");
    rejectsWithoutMutation("{\"code\":123456}");
    return 0;
}
