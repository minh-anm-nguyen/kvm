#include "board.h"

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "config.h"

static const board_pinmap_t PINMAP_A = {
    .uart_rx = BOARD_A_RX,
    .uart_tx = BOARD_A_TX,
    .usb_host_dp = BOARD_A_USB_DP,
};

static const board_pinmap_t PINMAP_B = {
    .uart_rx = BOARD_B_RX,
    .uart_tx = BOARD_B_TX,
    .usb_host_dp = BOARD_B_USB_DP,
};

static const bool probe_pattern[] = {
    1, 0, 0, 1,
    1, 0, 1, 0,
    0, 1, 0, 1,
    1, 1, 0, 0,
};

#define PROBE_PATTERN_LEN (sizeof(probe_pattern) / sizeof(probe_pattern[0]))
_Static_assert(PROBE_PATTERN_LEN > 0, "probe_pattern must not be empty");
_Static_assert(PROBE_PATTERN_LEN == 16, "probe_pattern must be length 16");

int dh_debug_printf(const char *format, ...) {
    (void)format;
    return 0;
}

bool board_role_is_concrete(board_role_t role) {
    return role == BOARD_ROLE_A || role == BOARD_ROLE_B;
}

const board_pinmap_t *board_get_pinmap(board_role_t role) {
    if (role == BOARD_ROLE_A) {
        return &PINMAP_A;
    }
    if (role == BOARD_ROLE_B) {
        return &PINMAP_B;
    }
    return NULL;
}

bool board_pinmap_selftest(void) {
    const board_pinmap_t *map_a = board_get_pinmap(BOARD_ROLE_A);
    const board_pinmap_t *map_b = board_get_pinmap(BOARD_ROLE_B);

    if (!board_role_is_concrete(BOARD_ROLE_A) ||
        !board_role_is_concrete(BOARD_ROLE_B)) {
        return false;
    }

    if (board_role_is_concrete(BOARD_ROLE_UNKNOWN) ||
        board_role_is_concrete(BOARD_ROLE_CONFLICT)) {
        return false;
    }

    if (map_a == NULL ||
        map_a->uart_rx != BOARD_A_RX ||
        map_a->uart_tx != BOARD_A_TX ||
        map_a->usb_host_dp != BOARD_A_USB_DP) {
        return false;
    }

    if (map_b == NULL ||
        map_b->uart_rx != BOARD_B_RX ||
        map_b->uart_tx != BOARD_B_TX ||
        map_b->usb_host_dp != BOARD_B_USB_DP) {
        return false;
    }

    if (board_get_pinmap(BOARD_ROLE_UNKNOWN) != NULL ||
        board_get_pinmap(BOARD_ROLE_CONFLICT) != NULL) {
        return false;
    }

    return true;
}

static void board_probe_apply_pull(uint pin, bool pull_high) {
    if (pull_high) {
        gpio_pull_up(pin);
    } else {
        gpio_pull_down(pin);
    }
}

static role_probe_result_t board_probe_classify(unsigned mismatches,
                                               unsigned sample_count) {
    if (sample_count == 0) {
        return PROBE_AMBIGUOUS;
    }

    if (mismatches == 0) {
        return PROBE_ROLE_B;
    }

    if (mismatches >= ROLE_A_MIN_MISMATCHES) {
        return PROBE_ROLE_A;
    }

    return PROBE_AMBIGUOUS;
}

role_probe_result_t board_probe_role_once(void) {
    unsigned mismatches = 0;

    gpio_init(BOARD_A_RX);
    gpio_set_dir(BOARD_A_RX, GPIO_IN);

    for (size_t i = 0; i < PROBE_PATTERN_LEN; i++) {
        board_probe_apply_pull(BOARD_A_RX, probe_pattern[i]);
        sleep_us(ROLE_PROBE_SETTLE_US);

        bool sampled = gpio_get(BOARD_A_RX);
        if (sampled != probe_pattern[i]) {
            mismatches++;
        }

        gpio_disable_pulls(BOARD_A_RX);
    }

    gpio_disable_pulls(BOARD_A_RX);

    return board_probe_classify(mismatches, (unsigned)PROBE_PATTERN_LEN);
}

bool board_probe_selftest(void) {
    unsigned ones = 0;
    unsigned zeros = 0;

    for (size_t i = 0; i < PROBE_PATTERN_LEN; i++) {
        if (probe_pattern[i]) {
            ones++;
        } else {
            zeros++;
        }
    }

    if (ones != zeros || ones != (PROBE_PATTERN_LEN / 2u)) {
        return false;
    }

    if (board_probe_classify(0, (unsigned)PROBE_PATTERN_LEN) != PROBE_ROLE_B) {
        return false;
    }

    if (board_probe_classify(1, (unsigned)PROBE_PATTERN_LEN) != PROBE_AMBIGUOUS ||
        board_probe_classify(2, (unsigned)PROBE_PATTERN_LEN) != PROBE_AMBIGUOUS ||
        board_probe_classify(3, (unsigned)PROBE_PATTERN_LEN) != PROBE_AMBIGUOUS) {
        return false;
    }

    if (board_probe_classify(ROLE_A_MIN_MISMATCHES,
                             (unsigned)PROBE_PATTERN_LEN) != PROBE_ROLE_A) {
        return false;
    }

    if (board_probe_classify(8, (unsigned)PROBE_PATTERN_LEN) != PROBE_ROLE_A ||
        board_probe_classify(16, (unsigned)PROBE_PATTERN_LEN) != PROBE_ROLE_A) {
        return false;
    }

    return true;
}

void board_init(device_state_t *state) {
    state->board_role       = (board_role_t)BOARD_ROLE;
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
    state->pointer_x        = POINTER_CENTER;
    state->pointer_y        = POINTER_CENTER;
    state->edge_switch_armed = true;
    state->peer_protocol_ok = false;
    state->protocol_mismatch = false;
    state->peer_protocol_version = 0;
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
    bool on;

    if (state->protocol_mismatch) {
        on = ((board_millis() / LED_PROTOCOL_MISMATCH_MS) & 1u) != 0;
    } else {
        on = (state->active_output == OUTPUT_B);
    }

    if (state->led_on != on) {
        state->led_on = on;
        gpio_put(GPIO_LED_PIN, on ? 1 : 0);
    }
}

void board_kick_watchdog(void) {
    watchdog_update();
}
