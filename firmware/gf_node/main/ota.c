/*
 * ota.c - the update path. STUB for Task 8: ota_handle() just fails every
 * request so the build links and /ota returns a clean 500 instead of a 404
 * or a hang. Task 9 replaces this with the real image-write handler; nothing
 * here is finished code.
 */
#include "esp_http_server.h"

esp_err_t ota_handle(httpd_req_t *req)
{
    return httpd_resp_send_500(req);
}
