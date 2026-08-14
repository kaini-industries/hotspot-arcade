#include "../hotspot_arcade_i.h"
#include "../helpers/ha_storage.h"
#include "../helpers/ha_session.h"

static void ha_ssid_input_cb(void* context) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HaEventSsidDone);
}

void hotspot_arcade_scene_ssid_input_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    strncpy(app->ssid_buf, furi_string_get_cstr(app->ssid), HA_SSID_MAX - 1);
    app->ssid_buf[HA_SSID_MAX - 1] = '\0';

    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Hotspot name (SSID)");
    text_input_set_result_callback(
        app->text_input, ha_ssid_input_cb, app, app->ssid_buf, HA_SSID_MAX, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, HaViewTextInput);
}

bool hotspot_arcade_scene_ssid_input_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == HaEventSsidDone) {
        if(strlen(app->ssid_buf) > 0) {
            bool changed = strcmp(app->ssid_buf, furi_string_get_cstr(app->ssid)) != 0;
            if(changed && app->session_active && !app->transport_paused &&
               app->hs == HaHsUp && app->portal_running && app->transport_network_ready) {
                // Keep the last proven SSID authoritative until the restarted ESP
                // reports `up`. If the new AP fails and falls back, host config and
                // dashboard therefore remain truthful without a compensating write.
                strlcpy(
                    app->transport_pending_ssid,
                    app->ssid_buf,
                    sizeof(app->transport_pending_ssid));
                ha_session_transport_pause(
                    app, HA_TRANSPORT_SSID_CHANGE, app->ssid_buf, 600000);
            } else if(changed && !app->session_active) {
                furi_string_set(app->ssid, app->ssid_buf);
                ha_storage_save_config(app);
            }
            // A live session that is still streaming/startup-recovering deliberately
            // ignores the edit. Its in-flight SET_AP owns the only safe candidate;
            // accepting a second one here could make host config diverge from the AP.
        }
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void hotspot_arcade_scene_ssid_input_on_exit(void* context) {
    UNUSED(context);
}
