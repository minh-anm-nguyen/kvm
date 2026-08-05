#include "board.h"

#include <stdarg.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "config.h"

int dh_debug_printf(const char *format, ...) {
    (void)format;
    return 0;
}

void board_init(device_state_t *state) {
    state->board_role       = (uint8_t)BOARD_ROLE;
    state->active_output    = DEFAULT_ACTIVE_OUTPUT;
    state->peer_online      = false;
    state->usb_device_ready = false;
    state->input_connected  = false;
    state->mouse_connected  = false;
    state->kbd_dev_addr     = 0;
    state->kbd_instance     = 0;
    state->mouse_dev_addr   = 0;
    state->mouse_instance   = 0;
    state->mouse_buttons    = 0;
    state->last_peer_heartbeat_ms = 0;
    state->output_generation = 0;
    state->led_on = false;
    memset(&state->local_keyboard, 0, sizeof(state->local_keyboard));
    memset(&state->remote_keyboard, 0, sizeof(state->remote_keyboard));

    gpio_init(GPIO_LED_PIN);
    gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
    gpio_put(GPIO_LED_PIN, state->active_output == OUTPUT_B ? 1 : 0);
    state->led_on = (state->active_output == OUTPUT_B);
}

void board_enable_watchdog(void) {
#ifdef KVM_DEBUG
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
#else
    watchdog_enable(WATCHDOG_TIMEOUT_MS, false);
#endif
}

uint32_t board_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

void board_update_led(device_state_t *state) {
    bool on = (state->active_output == OUTPUT_B);
    if (state->led_on != on) {
        state->led_on = on;
        gpio_put(GPIO_LED_PIN, on ? 1 : 0);
    }
}

void board_kick_watchdog(void) {
    watchdog_update();
}
