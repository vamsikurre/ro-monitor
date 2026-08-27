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

uint32_t rs485_error_count(void) { return s_errors; }

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
        return -1;
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
    return -1;
}
