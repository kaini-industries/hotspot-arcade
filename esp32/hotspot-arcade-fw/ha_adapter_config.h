#pragma once

#include "ha_json.h"

// Parse the optional admission code in an already validated flat CONFIG object.
// Empty explicitly disables code admission; otherwise exactly six ASCII digits
// are required. The destination is modified only on success so a rejected
// CONFIG cannot partially change live admission state.
static inline bool haParseJoinCodeExact(const char* json, char out[7]) {
    const char* value = ha_json_find(json, "code");
    if(!value || *value != '"') return false;
    value++;
    if(*value == '"') {
        out[0] = '\0';
        return true;
    }
    char parsed[7];
    for(int i = 0; i < 6; i++) {
        if(value[i] < '0' || value[i] > '9') return false;
        parsed[i] = value[i];
    }
    if(value[6] != '"') return false;
    parsed[6] = '\0';
    memcpy(out, parsed, sizeof(parsed));
    return true;
}
