#include "keyboard.h"

#include <string.h>

#include "tusb.h"

#include "config.h"
#include "usb_device.h"

typedef struct {
    hid_keyboard_report_t items[KEYBOARD_TX_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} kbd_queue_t;

static kbd_queue_t s_tx_q;

static bool report_is_empty(const hid_keyboard_report_t *r) {
    if (r->modifier != 0) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (r->keycode[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool queue_push(const hid_keyboard_report_t *report) {
    if (s_tx_q.count >= KEYBOARD_TX_QUEUE_LEN) {
        if (report_is_empty(report)) {
            s_tx_q.items[s_tx_q.head] = *report;
            return true;
        }
        s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % KEYBOARD_TX_QUEUE_LEN);
        s_tx_q.count--;
    }

    s_tx_q.items[s_tx_q.tail] = *report;
    s_tx_q.tail = (uint8_t)((s_tx_q.tail + 1) % KEYBOARD_TX_QUEUE_LEN);
    s_tx_q.count++;
    return true;
}

static bool queue_peek(hid_keyboard_report_t *report) {
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
    s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % KEYBOARD_TX_QUEUE_LEN);
    s_tx_q.count--;
}

void keyboard_init(void) {
    memset(&s_tx_q, 0, sizeof(s_tx_q));
    memset(&g_state.local_keyboard, 0, sizeof(g_state.local_keyboard));
}

static bool parse_boot_keyboard(const uint8_t *raw, uint16_t len, hid_keyboard_report_t *out) {
    if (raw == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (len >= 8) {
        out->modifier = raw[0];
        out->reserved = raw[1];
        memcpy(out->keycode, &raw[2], 6);
        return true;
    }

    return false;
}

void keyboard_on_report(const uint8_t *raw, uint16_t len) {
    hid_keyboard_report_t report;

    if (!parse_boot_keyboard(raw, len, &report)) {
        return;
    }

    g_state.local_keyboard = report;
    queue_push(&report);
}

void keyboard_on_unmount(void) {
    hid_keyboard_report_t empty = {0};
    g_state.local_keyboard = empty;
    queue_push(&empty);
}

void keyboard_task(device_state_t *state) {
    if (state->board_role != ROLE_A) {
        return;
    }

    hid_keyboard_report_t report;
    if (!queue_peek(&report)) {
        return;
    }

    if (!tud_mounted()) {
        return;
    }

    if (!tud_hid_n_ready(ITF_NUM_KEYBOARD)) {
        return;
    }

    if (usb_device_send_keyboard(&report)) {
        queue_pop();
    }
}
