/*
 * sensors_util.c - the utility node's half of the shared sensors_* interface.
 *
 * Task 8 stub: fixed values only, so the JSON shape is nailed down before any
 * sensing exists. Real CT-clamp, float-switch and SHT31 reading arrives in
 * Task 11. Guarded the same way as sensors_sump.c's #if/#endif, and for the
 * same reason: this file compiles into the sump build too, and must not
 * define sensors_init/sample/json there.
 */
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "gf.h"
#if CONFIG_GF_ROLE_UTILITY
void sensors_init(void) {}
void sensors_sample(void) {}
int sensors_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "{\"id\":6,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"bore_mv\":[0,0,0],\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":null,"
        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":false}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi());
}
#endif
