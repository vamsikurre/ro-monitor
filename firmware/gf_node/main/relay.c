/*
 * relay.c - the three relay outputs on the utility node.
 *
 * Relays are high-trigger with a 10 k pull-down on each input (WIRING.md
 * 11.5), so a pin that is high-Z through reset is off. relay_init() still
 * latches the level to 0 BEFORE making the pin an output so it never glitches
 * high between direction and level.
 */
#include "gf.h"

#if CONFIG_GF_ROLE_UTILITY
#include "driver/gpio.h"
#include "esp_timer.h"
#include "ctl.h"

#define START_PULSE_US 1000000

static esp_timer_handle_t s_pulse;
static bool s_start_prev;

static void pulse_end(void *arg) { (void)arg; gpio_set_level(GPIO_RLY_BORE_START, 0); }

void relay_init(void)
{
    static const int pins[3] = { GPIO_RLY_BORE_STOP, GPIO_RLY_BORE_START, GPIO_RLY_SUMP_STOP };
    /* Level first, then gpio_config (which also sets the IO_MUX function to GPIO; plain
     * gpio_set_direction does not). Not gpio_reset_pin: it enables the internal pull-up. */
    for (int i = 0; i < 3; i++) gpio_set_level(pins[i], 0);
    gpio_config_t io = { .pin_bit_mask = (1ULL << GPIO_RLY_BORE_STOP) | (1ULL << GPIO_RLY_BORE_START) | (1ULL << GPIO_RLY_SUMP_STOP),
                         .mode = GPIO_MODE_OUTPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
                         .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
    for (int i = 0; i < 3; i++) gpio_set_level(pins[i], 0);
    const esp_timer_create_args_t a = { .callback = pulse_end, .name = "start_pulse" };
    ESP_ERROR_CHECK(esp_timer_create(&a, &s_pulse));
}

/* Called under ctl_glue's mutex, so s_start_prev needs no lock of its own. */
void relay_apply(const ctl_out_t *o)
{
    gpio_set_level(GPIO_RLY_BORE_STOP, o->bore_stop);
    gpio_set_level(GPIO_RLY_SUMP_STOP, o->sump_stop);
    if (o->bore_stop) {                       /* a stop always wins over a pulse in flight */
        esp_timer_stop(s_pulse);
        gpio_set_level(GPIO_RLY_BORE_START, 0);
    } else if (o->bore_start && !s_start_prev) {
        esp_timer_stop(s_pulse);              /* restart the 1 s if one is still running */
        gpio_set_level(GPIO_RLY_BORE_START, 1);
        esp_timer_start_once(s_pulse, START_PULSE_US);
    }
    s_start_prev = o->bore_start;
}
#endif
