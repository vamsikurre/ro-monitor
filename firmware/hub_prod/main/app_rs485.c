/*
 * app_rs485.c — RS485 master
 *
 * Ported from the bench sketch's master, byte for byte on the wire. The XY-485
 * modules do auto direction switching, so there is no DE/RE line to drive and no
 * turnaround delay to get wrong — the only timing that matters is the guard gap
 * after a reply, before the master may transmit again.
 */

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_priv.h"
#include "app_rs485.h"

static const char *TAG = "rs485";
static uint32_t s_errors = 0;

/*
 * Per-node, per-command attribution.
 *
 * The aggregate counter said "err 300" and could not say anything else. Three
 * hundred failures on a wired bus reads as degradation, so it sends you to the
 * terminators and the trunk. It took dividing by uptime by hand to notice the
 * rate was exactly +2 every 20 s - dead regular, which no cable fault is - and
 * that 20 s is POLL_CYCLE_MS * WQ_POLL_CYCLES. The number was pointing at one
 * command on two nodes the whole time and had no way to say so.
 *
 * So: count polls as well as failures. A failure count alone cannot answer "is
 * this bad" - 148 failures is meaningless, 148/148 is a command that has never
 * once worked, and 3/1480 is a healthy bus. The ratio is the diagnosis.
 *
 * ponytail: fixed table, linear scan. Three nodes x a handful of commands fits
 * in 16 slots with room to spare; if it ever fills, the aggregate still counts
 * everything and only the breakdown loses the overflow. Per-node arrays if the
 * bus ever grows past a dozen node/command pairs.
 */
#define RS485_STAT_SLOTS 16

static struct {
    uint8_t  addr;
    uint8_t  cmd;
    uint32_t polls;      /* frames actually sent */
    uint32_t fails;      /* of those, ones that got no valid reply */
    uint32_t skips;      /* suppressed without sending - not counted as failures */
    uint16_t retry_in;   /* skips remaining before the next re-probe */
    bool     announced;  /* the "not answering this command" line is printed once */
} s_stat[RS485_STAT_SLOTS];
static uint8_t s_stat_used = 0;

/* Returns the slot index for this pair, or -1 when the table is full. */
static int stat_slot(uint8_t addr, uint8_t cmd)
{
    for (uint8_t i = 0; i < s_stat_used; i++) {
        if (s_stat[i].addr == addr && s_stat[i].cmd == cmd) {
            return i;
        }
    }
    if (s_stat_used >= RS485_STAT_SLOTS) {
        return -1;
    }
    s_stat[s_stat_used].addr = addr;
    s_stat[s_stat_used].cmd = cmd;
    return s_stat_used++;
}

uint32_t rs485_error_count(void) { return s_errors; }

/* Short mnemonic per command, so a log line reads without the protocol doc. */
static const char *cmd_word(uint8_t cmd);

/*
 * True when this poll should be suppressed rather than sent.
 *
 * Only ever true for a pair that has failed every single time it was tried - see
 * RS485_GIVEUP_POLLS. A pair that has answered even once is never suppressed,
 * however badly it is failing now, because that is a bus problem and retrying is
 * the right response to a bus problem.
 */
static bool suppress_poll(int i)
{
    if (s_stat[i].polls < RS485_GIVEUP_POLLS) {
        return false;
    }
    if (s_stat[i].fails != s_stat[i].polls) {
        return false;                     /* answered at least once: keep trying */
    }

    if (!s_stat[i].announced) {
        s_stat[i].announced = true;
        ESP_LOGW(TAG, "node 0x%02X never answers %s (%lu/%lu) - suppressing it and "
                      "re-probing every %d polls. A node running firmware older "
                      "than this command replies to nothing, so this is almost "
                      "certainly a node that needs reflashing.",
                 s_stat[i].addr, cmd_word(s_stat[i].cmd),
                 (unsigned long)s_stat[i].fails, (unsigned long)s_stat[i].polls,
                 RS485_LATCH_RETRY);
    }

    if (s_stat[i].retry_in > 0) {
        s_stat[i].retry_in--;
        return true;
    }
    /* Let this one through, then go quiet again. A reflashed node comes back on
     * its own; it does not need the hub restarted. */
    s_stat[i].retry_in = RS485_LATCH_RETRY;
    return false;
}

static const char *cmd_word(uint8_t cmd)
{
    switch (cmd) {
    case CMD_PING:          return "PING";
    case CMD_READ_LEVEL:    return "LEVEL";
    case CMD_READ_CLIMATE:  return "CLIMATE";
    case CMD_READ_WQ:       return "WQ";
    default:                return "CMD?";
    }
}

int rs485_error_report(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t at = 0;
    int shown = 0;

    for (uint8_t i = 0; i < s_stat_used; i++) {
        if (s_stat[i].fails == 0) {
            continue;                     /* silent when healthy */
        }
        /* Skips are reported separately from failures on purpose: a suppressed
         * poll was never sent, so folding it into either side of the ratio
         * would misreport the bus. "5/5 +148skip" is a command given up on;
         * "3/1480" is a bus dropping the odd frame. */
        int n;
        if (s_stat[i].skips > 0) {
            n = snprintf(buf + at, len - at, "%s0x%02X/%s %lu/%lu +%luskip",
                         shown ? "  " : "",
                         s_stat[i].addr, cmd_word(s_stat[i].cmd),
                         (unsigned long)s_stat[i].fails,
                         (unsigned long)s_stat[i].polls,
                         (unsigned long)s_stat[i].skips);
        } else {
            n = snprintf(buf + at, len - at, "%s0x%02X/%s %lu/%lu",
                         shown ? "  " : "",
                         s_stat[i].addr, cmd_word(s_stat[i].cmd),
                         (unsigned long)s_stat[i].fails,
                         (unsigned long)s_stat[i].polls);
        }
        if (n < 0 || (size_t)n >= len - at) {
            break;                        /* truncated: report what fitted */
        }
        at += (size_t)n;
        shown++;
    }
    return shown;
}

esp_err_t rs485_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, GPIO_RS485_TX, GPIO_RS485_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d up: %d baud, RX %d, TX %d",
             RS485_UART, RS485_BAUD, GPIO_RS485_RX, GPIO_RS485_TX);
    return ESP_OK;
}

uint16_t crc16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (uint8_t i = 8; i != 0; i--) {
            crc = (crc & 0x0001) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

int rs485_poll(uint8_t addr, uint8_t cmd, const uint8_t *req, uint8_t req_len, uint8_t *out)
{
    if (req_len > RS485_MAX_PAYLOAD || out == NULL) {
        return -1;   /* a caller bug, not a bus event: not counted either way */
    }

    /* Counted once per poll, not once per attempt. A poll that succeeds on its
     * third try is a working bus with a retry, and inflating the denominator by
     * three would hide exactly the ratio this table exists to show. */
    const int slot = stat_slot(addr, cmd);
    if (slot >= 0) {
        if (suppress_poll(slot)) {
            s_stat[slot].skips++;
            return -1;      /* absence, same as a timeout, and no frame sent */
        }
        s_stat[slot].polls++;
    }

    uint8_t frame[7 + RS485_MAX_PAYLOAD];
    frame[0] = PREAMBLE_1;
    frame[1] = PREAMBLE_2;
    frame[2] = addr;
    frame[3] = cmd;
    frame[4] = req_len;
    for (uint8_t i = 0; i < req_len; i++) {
        frame[5 + i] = req[i];
    }
    uint16_t req_crc = crc16(frame, (uint8_t)(5 + req_len));
    frame[5 + req_len] = (uint8_t)(req_crc & 0xFF);
    frame[6 + req_len] = (uint8_t)(req_crc >> 8);

    for (uint8_t attempt = 0; attempt < RS485_POLL_ATTEMPTS; attempt++) {
        uart_flush_input(RS485_UART);
        uart_write_bytes(RS485_UART, frame, (size_t)(7 + req_len));
        uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));

        uint8_t buf[7 + RS485_MAX_PAYLOAD];
        uint8_t n = 0;
        int64_t deadline = esp_timer_get_time() + (int64_t)RS485_REPLY_TIMEOUT_MS * 1000;

        while (esp_timer_get_time() < deadline) {
            uint8_t b;
            /* One byte at a time so a corrupt or truncated frame resynchronises
             * on the next preamble instead of poisoning the one after it. */
            if (uart_read_bytes(RS485_UART, &b, 1, pdMS_TO_TICKS(5)) != 1) {
                continue;
            }

            if (n == 0 && b != PREAMBLE_1) continue;              /* hunt for sync */
            if (n == 1 && b != PREAMBLE_2) { n = 0; continue; }
            if (n == 4 && b > RS485_MAX_PAYLOAD) { n = 0; continue; }
            buf[n++] = b;

            if (n >= 5) {
                uint8_t len = buf[4];
                if (n == (uint8_t)(7 + len)) {
                    uint16_t want = (uint16_t)buf[5 + len] | ((uint16_t)buf[6 + len] << 8);
                    n = 0;
                    if (crc16(buf, (uint8_t)(5 + len)) != want) continue;         /* corrupt */
                    if (buf[2] != addr) continue;                                 /* wrong node */
                    if (buf[3] != (uint8_t)(cmd | RESPONSE_BIT)) continue;        /* wrong reply */

                    memcpy(out, &buf[5], len);
                    vTaskDelay(pdMS_TO_TICKS(RS485_GUARD_MS));
                    return (int)len;
                }
            }
        }
    }

    s_errors++;
    if (slot >= 0) {
        s_stat[slot].fails++;
    }
    return -1;
}
