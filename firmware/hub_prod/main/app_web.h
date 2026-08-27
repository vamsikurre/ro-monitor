/*
 * app_web.h — local dashboard, telemetry API, and the calibration page
 *
 * Served on both interfaces. STA gives the dashboard a LAN address and
 * http://ro-hub.local; the hub's own AP keeps calibration reachable on a roof
 * with no router, which is the situation calibration actually happens in.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Routes:
 *   GET  /                 the dashboard (embedded, unauthenticated - read only)
 *   GET  /api/telemetry    the JSON the dashboard polls once a second
 *   GET  /cal              calibration form            [Basic auth]
 *   POST /api/cal/tank     full/empty distances        [Basic auth]
 *   POST /api/cal/ct       clamp scale, turns, OC trip [Basic auth]
 *   POST /api/cal/fan      fan on/off thresholds       [Basic auth]
 *   POST /api/cal/pass     change the calibration password [Basic auth]
 *
 * The dashboard is deliberately open and read-only: it is on the house LAN, it
 * changes nothing, and putting a password in front of a wall display is how
 * passwords end up written on the wall next to it. Everything that writes is
 * behind auth.
 */
esp_err_t web_start(void);

#ifdef __cplusplus
}
#endif
