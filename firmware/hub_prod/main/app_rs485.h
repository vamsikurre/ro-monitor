/*
 * app_rs485.h — RS485 master for the three Arduino nodes
 *
 * Frame: 0xAA 0x55 ADDR CMD LEN <payload> CRC_L CRC_H
 * CRC-16/Modbus over every preceding byte, payload fields big-endian.
 * docs/RS485_PROTOCOL.md is authoritative; docs/check_frame.py fails the build if
 * this file and firmware/ro_node/ro_node.ino disagree about the CRC or framing.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rs485_init(void);

/*
 * Send one request and wait for its reply. Retries up to RS485_POLL_ATTEMPTS.
 *
 * Returns the reply payload length, or -1 on timeout / CRC failure / a reply
 * from the wrong node. A negative return is absence, not zero — the caller must
 * not treat it as data, which is why the length is returned rather than a bool
 * with an out-parameter that may be stale.
 *
 * `out` must have room for RS485_MAX_PAYLOAD bytes.
 */
int rs485_poll(uint8_t addr, uint8_t cmd, const uint8_t *req, uint8_t req_len, uint8_t *out);

/* Cumulative failed polls since boot. Surfaced on the dashboard: a bus that works
 * but is degrading shows up here long before it shows up as an offline node. */
uint32_t rs485_error_count(void);

uint16_t crc16(const uint8_t *buf, uint8_t len);

#ifdef __cplusplus
}
#endif
