/*
 * sensors_sump.c - the sump node's half of the shared sensors_* interface.
 *
 * Task 8 stub: fixed values only, so the JSON shape is nailed down before any
 * sensing exists. Real ultrasonic/loop reading arrives in Task 10. This file
 * still compiles into the utility build (main/CMakeLists.txt lists both
 * sensors_*.c unconditionally so neither role's CMakeLists needs to change
 * when the other role is picked) - the #if/#endif around the whole body is
 * what keeps it from also defining sensors_init/sample/json there and
 * colliding with sensors_util.c's copies.
 */
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "gf.h"
#if CONFIG_GF_ROLE_SUMP
void sensors_init(void) {}
void sensors_sample(void) {}
int sensors_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "{\"id\":5,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"source\":\"ultrasonic\",\"distance_mm\":null,\"quality\":null,\"status\":\"NO_ECHO\",\"loop_ua\":null}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi());
}
#endif
