#include "mouse.h"

#include <string.h>

#include "tusb.h"

#include "config.h"
#include "router.h"
#include "uart.h"
#include "usb_device.h"

typedef struct {
    mouse_rel_report_t items[MOUSE_TX_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} mouse_queue_t;

static mouse_queue_t s_tx_q;

static bool queue_push(const mouse_rel_report_t *report) {
    if (s_tx_q.count >= MOUSE_TX_QUEUE_LEN) {
        s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % MOUSE_TX_QUEUE_LEN);
        s_tx_q.count--;
    }

    s_tx_q.items[s_tx_q.tail] = *report;
    s_tx_q.tail = (uint8_t)((s_tx_q.tail + 1) % MOUSE_TX_QUEUE_LEN);
    s_tx_q.count++;
    return true;
}

static bool queue_peek(mouse_rel_report_t *report) {
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

void mouse_queue_local(const mouse_rel_report_t *report) {
    if (report == NULL) {
        return;
    }
    queue_push(report);
}

void mouse_release_local(void) {
    mouse_rel_report_t empty = {0};
    g_state.mouse_buttons = 0;
    queue_push(&empty);
}

void mouse_init(void) {
    memset(&s_tx_q, 0, sizeof(s_tx_q));
    g_state.mouse_buttons = 0;
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

static void mouse_route(device_state_t *state, const mouse_rel_report_t *report) {
    if (state->board_role != ROLE_B) {
        return;
    }

    if (state->active_output == OUTPUT_B) {
        mouse_queue_local(report);
    } else {
        uart_queue_mouse(report);
    }
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

    g_state.mouse_buttons = report.buttons;
    mouse_route(&g_state, &report);
}

void mouse_on_unmount(void) {
    mouse_rel_report_t empty = {0};
    g_state.mouse_buttons = 0;
    mouse_route(&g_state, &empty);
}

void mouse_task(device_state_t *state) {
    mouse_rel_report_t report;
    if (!queue_peek(&report)) {
        return;
    }

    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_MOUSE)) {
        return;
    }

    if (usb_device_send_mouse(report.buttons, report.x, report.y, report.wheel)) {
        queue_pop();
    }

    (void)state;
}
