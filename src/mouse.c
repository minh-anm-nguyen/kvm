#include "mouse.h"

#include <string.h>

#include "tusb.h"

#include "config.h"
#include "uart.h"
#include "usb_device.h"

typedef struct {
    mouse_abs_report_t items[MOUSE_TX_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} mouse_queue_t;

static mouse_queue_t s_tx_q;
static uint8_t s_last_buttons;
static uint16_t s_last_x;
static uint16_t s_last_y;
static int8_t s_last_wheel;
static bool s_have_last;

int32_t mouse_scale_delta(int8_t delta, int32_t scale) {
    return (int32_t)delta * scale;
}

int32_t mouse_clamp(int32_t value, int32_t lo, int32_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void mouse_pointer_advance(int32_t *pointer_x,
                           int32_t *pointer_y,
                           int8_t dx,
                           int8_t dy) {
    if (pointer_x == NULL || pointer_y == NULL) {
        return;
    }

    int32_t next_x = *pointer_x + mouse_scale_delta(dx, POINTER_SCALE_X);
    int32_t next_y = *pointer_y + mouse_scale_delta(dy, POINTER_SCALE_Y);

    *pointer_x = mouse_clamp(next_x, POINTER_MIN, POINTER_MAX);
    *pointer_y = mouse_clamp(next_y, POINTER_MIN, POINTER_MAX);
}

void mouse_build_report(const device_state_t *state,
                        uint8_t buttons,
                        int8_t wheel,
                        mouse_abs_report_t *out) {
    if (state == NULL || out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->buttons = buttons;
    out->x = (uint16_t)mouse_clamp(state->pointer_x, POINTER_MIN, POINTER_MAX);
    out->y = (uint16_t)mouse_clamp(state->pointer_y, POINTER_MIN, POINTER_MAX);
    out->wheel = wheel;
}

static bool report_unchanged(const mouse_abs_report_t *report) {
    if (!s_have_last) {
        return false;
    }
    return report->buttons == s_last_buttons &&
           report->x == s_last_x &&
           report->y == s_last_y &&
           report->wheel == s_last_wheel;
}

static void remember_report(const mouse_abs_report_t *report) {
    s_last_buttons = report->buttons;
    s_last_x = report->x;
    s_last_y = report->y;
    s_last_wheel = report->wheel;
    s_have_last = true;
}

static bool queue_push(const mouse_abs_report_t *report) {
    if (s_tx_q.count >= MOUSE_TX_QUEUE_LEN) {
        if (report->buttons == 0) {
            s_tx_q.items[s_tx_q.head] = *report;
            return true;
        }
        s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % MOUSE_TX_QUEUE_LEN);
        s_tx_q.count--;
    }

    s_tx_q.items[s_tx_q.tail] = *report;
    s_tx_q.tail = (uint8_t)((s_tx_q.tail + 1) % MOUSE_TX_QUEUE_LEN);
    s_tx_q.count++;
    return true;
}

static bool queue_peek(mouse_abs_report_t *report) {
    if (s_tx_q.count == 0) {
        return false;
    }
    *report = s_tx_q.items[s_tx_q.head];
    return true;
}

static void queue_pop(void) {
    if (s_tx_q.count == 0) {
        return;
    }
    s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % MOUSE_TX_QUEUE_LEN);
    s_tx_q.count--;
}

void mouse_queue_local(const mouse_abs_report_t *report) {
    if (report == NULL) {
        return;
    }
    queue_push(report);
}

void mouse_release_local(void) {
    mouse_abs_report_t empty;
    mouse_build_report(&g_state, 0, 0, &empty);
    g_state.mouse_buttons = 0;
    s_have_last = false;
    queue_push(&empty);
}

void mouse_init(void) {
    memset(&s_tx_q, 0, sizeof(s_tx_q));
    g_state.mouse_buttons = 0;
    g_state.pointer_x = POINTER_CENTER;
    g_state.pointer_y = POINTER_CENTER;
    g_state.edge_switch_armed = true;
    s_have_last = false;
    s_last_buttons = 0;
    s_last_x = 0;
    s_last_y = 0;
    s_last_wheel = 0;
}

static bool parse_boot_mouse(const uint8_t *raw, uint16_t len, mouse_rel_report_t *out) {
    if (raw == NULL || out == NULL || len < 3) {
        return false;
    }

    out->buttons = raw[0];
    out->x = (int8_t)raw[1];
    out->y = (int8_t)raw[2];
    out->wheel = (len >= 4) ? (int8_t)raw[3] : 0;
    return true;
}

static void mouse_route_absolute(device_state_t *state, const mouse_abs_report_t *report) {
    if (state->board_role != ROLE_B) {
        return;
    }

    if (report_unchanged(report)) {
        return;
    }
    remember_report(report);

    if (state->active_output == OUTPUT_B) {
        mouse_queue_local(report);
    } else {
        uart_queue_mouse(report);
    }
}

static void mouse_process_relative(device_state_t *state,
                                   int8_t dx,
                                   int8_t dy,
                                   uint8_t buttons,
                                   int8_t wheel) {
    if (state->board_role != ROLE_B) {
        return;
    }

    mouse_pointer_advance(&state->pointer_x, &state->pointer_y, dx, dy);
    state->mouse_buttons = buttons;

    mouse_abs_report_t report;
    mouse_build_report(state, buttons, wheel, &report);
    mouse_route_absolute(state, &report);
}

void mouse_on_report(const uint8_t *raw, uint16_t len) {
    mouse_rel_report_t report;

    if (!parse_boot_mouse(raw, len, &report)) {
        return;
    }

    if (report.x == 0 && report.y == 0 && report.wheel == 0 &&
        report.buttons == g_state.mouse_buttons) {
        return;
    }

    mouse_process_relative(&g_state, report.x, report.y, report.buttons, report.wheel);
}

void mouse_on_unmount(void) {
    g_state.mouse_buttons = 0;
    mouse_process_relative(&g_state, 0, 0, 0, 0);
}

void mouse_task(device_state_t *state) {
    mouse_abs_report_t report;
    if (!queue_peek(&report)) {
        return;
    }

    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_MOUSE)) {
        return;
    }

    if (usb_device_send_mouse(&report)) {
        queue_pop();
    }

    (void)state;
}

bool mouse_pointer_selftest(void) {
    /* Positive movement */
    if (mouse_scale_delta(2, POINTER_SCALE_X) != 2 * POINTER_SCALE_X) {
        return false;
    }
    if (mouse_scale_delta(1, POINTER_SCALE_Y) != POINTER_SCALE_Y) {
        return false;
    }

    /* Negative movement */
    if (mouse_scale_delta(-3, POINTER_SCALE_X) != -3 * POINTER_SCALE_X) {
        return false;
    }
    if (mouse_clamp(mouse_scale_delta(-5, POINTER_SCALE_Y), POINTER_MIN, POINTER_MAX) !=
        mouse_clamp(-5 * POINTER_SCALE_Y, POINTER_MIN, POINTER_MAX)) {
        return false;
    }

    /* Clamp bounds */
    if (mouse_clamp(POINTER_MIN - 100, POINTER_MIN, POINTER_MAX) != POINTER_MIN) {
        return false;
    }
    if (mouse_clamp(POINTER_MAX + 100, POINTER_MIN, POINTER_MAX) != POINTER_MAX) {
        return false;
    }
    if (mouse_clamp(POINTER_CENTER, POINTER_MIN, POINTER_MAX) != POINTER_CENTER) {
        return false;
    }

    /* Zero movement keeps position */
    {
        int32_t x = POINTER_CENTER;
        int32_t y = POINTER_CENTER;
        mouse_pointer_advance(&x, &y, 0, 0);
        if (x != POINTER_CENTER || y != POINTER_CENTER) {
            return false;
        }
    }

    /* Large positive overflow clamps to MAX */
    {
        int32_t x = POINTER_MAX - 10;
        int32_t y = POINTER_CENTER;
        mouse_pointer_advance(&x, &y, 127, 0);
        if (x != POINTER_MAX) {
            return false;
        }
    }

    /* Large negative overflow clamps to MIN */
    {
        int32_t x = POINTER_MIN + 10;
        int32_t y = POINTER_CENTER;
        mouse_pointer_advance(&x, &y, -128, 0);
        if (x != POINTER_MIN) {
            return false;
        }
    }

    /* Intermediate scale from center */
    {
        int32_t x = POINTER_CENTER;
        int32_t y = POINTER_CENTER;
        mouse_pointer_advance(&x, &y, 1, -1);
        if (x != POINTER_CENTER + POINTER_SCALE_X) {
            return false;
        }
        if (y != POINTER_CENTER - POINTER_SCALE_Y) {
            return false;
        }
    }

    return true;
}
