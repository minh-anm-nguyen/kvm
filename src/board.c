#include "board.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

void board_init(device_state_t *state) {
    state->board_role    = (uint8_t)BOARD_ROLE;
    state->active_output = DEFAULT_ACTIVE_OUTPUT;
    state->peer_online   = false;
    state->usb_device_ready = false;
    state->input_connected = false;
    state->mouse_buttons = 0;
    state->last_peer_heartbeat_ms = 0;
    state->output_generation = 0;
    state->led_on = false;

    gpio_init(GPIO_LED_PIN);
    gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
    gpio_put(GPIO_LED_PIN, 0);
}

uint32_t board_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

void board_update_led(device_state_t *state) {
    const uint32_t period_ms =
        (state->board_role == ROLE_A) ? LED_BLINK_MS_ROLE_A : LED_BLINK_MS_ROLE_B;

    static uint32_t last_toggle_ms;
    uint32_t now = board_millis();

    if ((now - last_toggle_ms) >= period_ms) {
        last_toggle_ms = now;
        state->led_on = !state->led_on;
        gpio_put(GPIO_LED_PIN, state->led_on ? 1 : 0);
    }
}

void board_kick_watchdog(void) {
    /* No-op until Step 7 enables the hardware watchdog. */
}
