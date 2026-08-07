#include "keyboard.h"

#include <string.h>

#include "tusb.h"

#include "config.h"
#include "board.h"
#include "router.h"
#include "uart.h"
#include "usb_device.h"

typedef struct {
    hid_keyboard_report_t items[KEYBOARD_TX_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} kbd_queue_t;

static kbd_queue_t s_tx_q;
static bool s_hotkey_held;

/*
 * Hotkey safety gate: true after a new Ctrl+Caps Lock switch is accepted.
 *
 * While true, keyboard_on_report() discards every non-empty report.  It resets
 * only when it receives an all-keys-released report (or the keyboard unmounts),
 * preventing the hotkey's remaining key-up/key-down state from reaching the
 * newly selected PC.
 */
static bool s_suppress_until_empty;

/*
 * Determine whether a boot-keyboard report represents "all keys released".
 *
 * Input:
 *   r  non-NULL HID keyboard report containing modifier bits and six keycode
 *      slots.
 *
 * Output:
 *   true only when no modifier is held and every keycode slot is zero.  Queue
 *   overflow and hotkey suppression use this to preserve release reports.
 */
static bool report_is_empty(const hid_keyboard_report_t *r) {
    if (r->modifier != 0) {
        /* Any Ctrl/Shift/Alt/GUI modifier means a key is still held. */
        return false;
    }

    for (int i = 0; i < 6; i++) {
        /* Boot protocol carries up to six non-modifier keys in these slots. */
        if (r->keycode[i] != 0) {
            /* A non-zero usage code means this is not an all-keys-up report. */
            return false;
        }
    }

    return true;
}

/*
 * Search the six non-modifier keycode slots of a boot-keyboard report.
 *
 * Input:
 *   key     HID usage code to find.
 *   report  non-NULL decoded keyboard report.
 *
 * Output:
 *   true when key appears in any keycode slot; false otherwise.  Modifier bits
 *   are intentionally not inspected here because callers check them separately.
 */
static bool key_in_report(uint8_t key, const hid_keyboard_report_t *report) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == key) {
            /* Found the requested HID usage code. */
            return true;
        }
    }

    return false;
}

/*
 * Recognize the fixed keyboard shortcut that switches the active output.
 *
 * Input:
 *   report  non-NULL decoded boot-keyboard report.
 *
 * Output:
 *   true when all bits in HOTKEY_MODIFIER are held and HOTKEY_KEYCODE appears
 *   in the non-modifier keycode slots.  Additional modifiers or keys do not
 *   prevent recognition because the required combination is still present.
 */
static bool is_switch_hotkey(const hid_keyboard_report_t *report) {
    if ((report->modifier & HOTKEY_MODIFIER) != HOTKEY_MODIFIER) {
        /* The required modifier, currently Left Ctrl, is not held. */
        return false;
    }

    /* Require the non-modifier hotkey, currently Caps Lock, as well. */
    return key_in_report(HOTKEY_KEYCODE, report);
}

/*
 * Append one HID keyboard report to the fixed-size circular queue consumed by
 * keyboard_task().
 *
 * Purpose:
 *   Decouple keyboard routing from USB HID readiness so reports can wait until
 *   the local device endpoint accepts them.
 *
 * Input:
 *   report  non-NULL HID keyboard report to copy into the queue.
 *
 * Output:
 *   Updates s_tx_q and always returns true.  When full, ordinary reports drop
 *   the oldest entry, but an all-keys-released report replaces the front entry
 *   so releases are sent next and cannot be lost.
 */
static bool queue_push(const hid_keyboard_report_t *report) {
    if (s_tx_q.count >= KEYBOARD_TX_QUEUE_LEN) {
        /* Queue is full: apply the overflow policy before appending. */
        if (report_is_empty(report)) {
            /* Key releases are safety-critical; make this report the next sent. */
            s_tx_q.items[s_tx_q.head] = *report;
            return true;
        }

        /* For ordinary key state, discard the oldest queued report. */
        s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % KEYBOARD_TX_QUEUE_LEN);
        s_tx_q.count--;
    }

    s_tx_q.items[s_tx_q.tail] = *report;
    /* Copy the report into the current write slot. */

    s_tx_q.tail = (uint8_t)((s_tx_q.tail + 1) % KEYBOARD_TX_QUEUE_LEN);
    /* Advance tail with wraparound to preserve the ring-buffer layout. */

    s_tx_q.count++;
    /* Account for the newly queued keyboard report. */

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

void keyboard_queue_local(const hid_keyboard_report_t *report) {
    if (report == NULL) {
        return;
    }
    queue_push(report);
}

void keyboard_init(void) {
    memset(&s_tx_q, 0, sizeof(s_tx_q));
    memset(&g_state.local_keyboard, 0, sizeof(g_state.local_keyboard));
    memset(&g_state.remote_keyboard, 0, sizeof(g_state.remote_keyboard));
    s_hotkey_held = false;
    s_suppress_until_empty = false;
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

static void keyboard_route(device_state_t *state, const hid_keyboard_report_t *report) {
    if (!board_accepts_keyboard(state->board_role)) {
        return;
    }

    if (state->active_output == OUTPUT_A) {
        keyboard_queue_local(report);
    } else {
        uart_queue_keyboard(report);
    }
}

/*
 * Accept one raw USB HID keyboard report from the host callback, handle the
 * fixed output-switch hotkey, and route ordinary keyboard state onward.
 *
 * Purpose:
 *   Board A owns the physical keyboard.  This is the first stage of its input
 *   path: it decodes the boot report, prevents the switching hotkey from being
 *   typed into either PC, and routes all other reports to the active output.
 *
 * Input:
 *   raw  bytes supplied by TinyUSB for a boot-keyboard input report.
 *   len  number of valid bytes in raw.
 *
 * Output:
 *   Updates g_state.local_keyboard.  A hotkey toggles active_output exactly
 *   once per hold; normal reports are passed to keyboard_route().  Reports are
 *   suppressed until an all-keys-released report follows a hotkey switch.
 */
void keyboard_on_report(const uint8_t *raw, uint16_t len) {
    hid_keyboard_report_t report;
    /* Temporary decoded modifier and keycode state from the HID input bytes. */

    if (!g_state.routing_enabled) {
        return;
    }

    if (!parse_boot_keyboard(raw, len, &report)) {
        /* Reject malformed or undersized raw HID reports. */
        return;
    }

    g_state.local_keyboard = report;
    /* Preserve the latest physical keyboard state before special-case handling. */

    if (is_switch_hotkey(&report)) {
        /* Left Ctrl + Caps Lock is reserved for switching, never normal typing. */
        if (!s_hotkey_held) {
            /* First report of this hold: avoid toggling repeatedly while held. */
            s_hotkey_held = true;

            s_suppress_until_empty = true;
            /* Do not forward follow-up hotkey/release reports until all keys lift. */

            uint8_t next = (uint8_t)(g_state.active_output ^ 1u);
            /* XOR 1 toggles the two valid outputs: A <-> B. */

            router_set_active_output(&g_state, next, true, SWITCH_REASON_HOTKEY);
            /* Release the old output, select next, increment generation, notify peer. */
        }

        return;
        /* Never route the reserved hotkey itself to a PC. */
    }

    s_hotkey_held = false;
    /* A non-hotkey report means the hotkey combination is no longer held. */

    if (s_suppress_until_empty) {
        /* Wait for a clean all-keys-up report after switching outputs. */
        if (!report_is_empty(&report)) {
            /* Suppress remaining keys while the user releases the hotkey. */
            return;
        }

        s_suppress_until_empty = false;
        /* The empty report is safe to route and ends hotkey suppression. */
    }

    keyboard_route(&g_state, &report);
    /* Queue locally or send over UART according to the active output. */
}

void keyboard_on_unmount(void) {
    hid_keyboard_report_t empty = {0};
    g_state.local_keyboard = empty;
    s_hotkey_held = false;
    s_suppress_until_empty = false;
    if (!g_state.routing_enabled) {
        return;
    }
    keyboard_route(&g_state, &empty);
}

void keyboard_task(device_state_t *state) {
    hid_keyboard_report_t report;
    if (!queue_peek(&report)) {
        return;
    }

    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_KEYBOARD)) {
        return;
    }

    if (usb_device_send_keyboard(&report)) {
        queue_pop();
    }

    (void)state;
}
