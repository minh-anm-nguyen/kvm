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

/*
 * Convert a signed relative mouse delta into the internal absolute-coordinate
 * increment for one axis.
 *
 * Purpose:
 *   Relative HID mice report small signed dx/dy values.  The pointer state uses
 *   a larger absolute coordinate range, so each delta must be scaled first.
 *
 * Input:
 *   delta  signed movement reported by the mouse for one axis.
 *   scale  number of absolute-coordinate units represented by one delta unit.
 *
 * Output:
 *   The signed 32-bit product delta * scale.  This helper does not clamp the
 *   result; mouse_pointer_advance() clamps the accumulated coordinate instead.
 */
int32_t mouse_scale_delta(int8_t delta, int32_t scale) {
    /* Promote delta before multiplying so the result cannot overflow int8_t. */
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

/*
 * Advance the internal absolute pointer position by one relative HID movement.
 *
 * Purpose:
 *   Translate dx/dy from a physical mouse into the bounded absolute pointer
 *   position that can be serialized identically for the local and peer paths.
 *
 * Input:
 *   pointer_x, pointer_y  writable current coordinates.  Both must be non-NULL.
 *   dx, dy                signed relative movement reported by the mouse.
 *
 * Output:
 *   Replaces *pointer_x and *pointer_y with their scaled, clamped next values.
 *   If either coordinate pointer is NULL, it returns without changing anything.
 */
void mouse_pointer_advance(int32_t *pointer_x,
                           int32_t *pointer_y,
                           int8_t dx,
                           int8_t dy) {
    if (pointer_x == NULL || pointer_y == NULL) {
        /* Both axes are required; avoid dereferencing an invalid pointer. */
        return;
    }

    /* Scale each relative axis before adding it to its absolute coordinate. */
    int32_t next_x = *pointer_x + mouse_scale_delta(dx, POINTER_SCALE_X);
    int32_t next_y = *pointer_y + mouse_scale_delta(dy, POINTER_SCALE_Y);

    /* Keep the canonical pointer within the range accepted by the HID report. */
    *pointer_x = mouse_clamp(next_x, POINTER_MIN, POINTER_MAX);
    *pointer_y = mouse_clamp(next_y, POINTER_MIN, POINTER_MAX);
}

/*
 * Construct one canonical absolute mouse report from the current device state.
 *
 * Purpose:
 *   Capture buttons, wheel movement, and the accumulated pointer position in a
 *   single report suitable for either local USB output or UART transmission.
 *
 * Input:
 *   state    non-NULL state containing the current signed pointer_x/pointer_y.
 *   buttons  current HID mouse-button bit mask.
 *   wheel    signed wheel delta for this report.
 *   out      non-NULL destination mouse_abs_report_t to populate.
 *
 * Output:
 *   Clears and fills *out with buttons, clamped unsigned x/y, and wheel.  The
 *   function never changes *state; if state or out is NULL, it does nothing.
 */
void mouse_build_report(const device_state_t *state,
                        uint8_t buttons,
                        int8_t wheel,
                        mouse_abs_report_t *out) {
    if (state == NULL || out == NULL) {
        /* A source state and destination report are both required. */
        return;
    }

    /* Zero reserved/padding fields before assigning the wire-visible values. */
    memset(out, 0, sizeof(*out));
    out->buttons = buttons;
    /* Clamp again defensively before converting the signed state to uint16_t. */
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

/*
 * Append an absolute mouse report to the fixed-size circular queue consumed by
 * mouse_task().
 *
 * Purpose:
 *   Decouple mouse routing from USB HID readiness.  Reports can be produced
 *   while the USB endpoint is busy and sent later by the task loop.
 *
 * Input:
 *   report  non-NULL absolute mouse report to copy into the queue.
 *
 * Output:
 *   Updates s_tx_q and always returns true.  On overflow, normal movement
 *   drops the oldest report, while a button-release report replaces the front
 *   entry so the release is transmitted next and is not lost.
 */
static bool queue_push(const mouse_abs_report_t *report) {
    if (s_tx_q.count >= MOUSE_TX_QUEUE_LEN) {
        /* Queue is full: apply the overflow policy before appending. */
        if (report->buttons == 0) {
            /* Releases are safety-critical; make this report the next one sent. */
            s_tx_q.items[s_tx_q.head] = *report;
            return true;
        }

        /* For ordinary movement, discard the oldest queued report. */
        s_tx_q.head = (uint8_t)((s_tx_q.head + 1) % MOUSE_TX_QUEUE_LEN);
        s_tx_q.count--;
    }

    s_tx_q.items[s_tx_q.tail] = *report;
    /* Copy the report into the current write slot. */

    s_tx_q.tail = (uint8_t)((s_tx_q.tail + 1) % MOUSE_TX_QUEUE_LEN);
    /* Advance tail with wraparound to preserve the ring-buffer layout. */

    s_tx_q.count++;
    /* Account for the new queued report. */

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

/*
 * Decode the relative fields this firmware accepts from a USB HID boot-mouse
 * input report.
 *
 * Input:
 *   raw[0]     button bitmap
 *   raw[1]     signed relative X delta (dx)
 *   raw[2]     signed relative Y delta (dy)
 *   raw[3]     optional signed vertical-wheel delta
 *   len        number of bytes available in raw; a report needs at least
 *              button, X, and Y bytes to be useful.
 *
 * Output:
 *   out        receives the decoded relative mouse report.  The caller turns
 *              these relative deltas into the absolute pointer position used
 *              by the rest of the firmware.
 *
 * Return value:
 *   true       raw was valid and out was populated.
 *   false      a required pointer was NULL or the report was too short.
 */
static bool parse_boot_mouse(const uint8_t *raw, uint16_t len, mouse_rel_report_t *out) {
    if (raw == NULL || out == NULL || len < 3) {
        /* Need an input buffer, an output object, and buttons + X + Y. */
        return false;
    }

    /* Byte 0 is copied as-is because each bit represents a mouse button. */
    out->buttons = raw[0];

    /* Bytes 1 and 2 are signed relative movements, not absolute positions. */
    out->x = (int8_t)raw[1];
    out->y = (int8_t)raw[2];

    /* Wheel is optional; three-byte reports have no wheel field. */
    out->wheel = (len >= 4) ? (int8_t)raw[3] : 0;

    return true;
}

/*
 * Route one absolute mouse report from the board-B mouse owner to the active
 * PC.
 *
 * Input:
 *   state      shared device state.  board_role identifies which board is
 *              executing and active_output identifies the selected PC.
 *   report     absolute X/Y position plus button and wheel state, built after
 *              the relative boot-mouse movement has been accumulated.
 *
 * Output:
 *   When B is active, queue report for the local USB device on board B.
 *   When A is active, queue MSG_MOUSE_REPORT for UART delivery to board A.
 *   Repeated reports produce no output, avoiding redundant USB/UART traffic.
 *
 * Only board B routes mouse input because it is the sole owner of the
 * physical mouse and of the accumulated pointer coordinates.
 */
static void mouse_route_absolute(device_state_t *state, const mouse_abs_report_t *report) {
    if (state->board_role != ROLE_B) {
        /* Board A receives remote reports; it must not create a second route. */
        return;
    }

    if (report_unchanged(report)) {
        /* Buttons, X, Y, and wheel are unchanged, so no output is needed. */
        return;
    }

    /* Remember the report before routing so the next identical one is skipped. */
    remember_report(report);

    if (state->active_output == OUTPUT_B) {
        /* PC B is active: its USB device is attached to this board. */
        mouse_queue_local(report);
    } else {
        /* PC A is active: send the absolute report to board A over UART. */
        uart_queue_mouse(report);
    }
}

/*
 * Convert one relative boot-mouse event into the absolute report consumed by
 * the routing layer.
 *
 * Input:
 *   state      valid shared device state.  Board B owns pointer_x/pointer_y.
 *   dx, dy     signed relative movement from the physical boot mouse.
 *   buttons    current button bitmap from that report.
 *   wheel      signed vertical-wheel delta from that report.
 *
 * Output:
 *   Updates board B's accumulated absolute pointer position and button state,
 *   then passes a mouse_abs_report_t to mouse_route_absolute().  That next
 *   step queues it for local USB or UART depending on active_output.
 *
 * Board A never performs this conversion: accepting movement there would give
 * the two boards independent pointer accumulators that could drift apart.
 */
static void mouse_process_relative(device_state_t *state,
                                   int8_t dx,
                                   int8_t dy,
                                   uint8_t buttons,
                                   int8_t wheel) {
    if (state->board_role != ROLE_B) {
        /* Only board B owns the physical mouse and pointer accumulator. */
        return;
    }

    /* Scale dx/dy, add them to X/Y, then clamp within the absolute HID range. */
    mouse_pointer_advance(&state->pointer_x, &state->pointer_y, dx, dy);

    /* Retain the latest button state for change detection and release handling. */
    state->mouse_buttons = buttons;

    mouse_abs_report_t report;
    /* Build an 8-byte absolute report from the updated state and wheel delta. */
    mouse_build_report(state, buttons, wheel, &report);

    /* Route it to PC B locally or to PC A through UART. */
    mouse_route_absolute(state, &report);
}

/*
 * Accept one raw USB HID mouse report from the host callback and start the
 * relative-to-absolute mouse pipeline.
 *
 * Input:
 *   raw        bytes supplied by TinyUSB for the boot-mouse input report.
 *   len        number of valid bytes in raw.
 *
 * Output:
 *   No direct return value.  A valid report with a movement, wheel, or button
 *   change is forwarded to mouse_process_relative(), which updates pointer
 *   state and eventually queues USB or UART output.  Repeated idle reports
 *   are intentionally discarded.
 */
void mouse_on_report(const uint8_t *raw, uint16_t len) {
    mouse_rel_report_t report;
    /* Temporary decoded boot-mouse data: buttons, relative X/Y, and wheel. */

    if (!parse_boot_mouse(raw, len, &report)) {
        /* Reject NULL or undersized raw HID reports before reading their fields. */
        return;
    }

    if (report.x == 0 && report.y == 0 && report.wheel == 0 &&
        report.buttons == g_state.mouse_buttons) {
        /* Nothing changed since the previous state, so avoid redundant output. */
        return;
    }

    /* Convert the relative event to an absolute report and route it onward. */
    mouse_process_relative(&g_state, report.x, report.y, report.buttons, report.wheel);
}

void mouse_on_unmount(void) {
    g_state.mouse_buttons = 0;
    mouse_process_relative(&g_state, 0, 0, 0, 0);
}

/*
 * Drain at most one pending absolute mouse report from the local USB-output
 * queue.  main() calls this task repeatedly, so retaining an unsent report
 * lets a later iteration retry after USB becomes available.
 *
 * Input:
 *   state      currently unused; kept in the task interface so it matches the
 *              other device tasks and can accept future state-based policy.
 *   s_tx_q     internal queue populated by mouse_queue_local().
 *
 * Output:
 *   On a mounted, ready HID mouse interface, sends one report to the local
 *   PC and removes it from s_tx_q only after usb_device_send_mouse() succeeds.
 */
void mouse_task(device_state_t *state) {
    mouse_abs_report_t report;

    if (!queue_peek(&report)) {
        /* Queue is empty: there is no local USB mouse report to send. */
        return;
    }

    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_MOUSE)) {
        /* Keep the report queued until the USB device is mounted and ready. */
        return;
    }

    if (usb_device_send_mouse(&report)) {
        /* Remove it only after TinyUSB accepted the HID report. */
        queue_pop();
    }

    /* State is intentionally unused by the current queue-draining policy. */
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
