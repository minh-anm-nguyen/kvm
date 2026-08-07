#include "router.h"

#include <string.h>

#include "board.h"
#include "config.h"
#include "keyboard.h"
#include "mouse.h"
#include "protocol.h"
#include "uart.h"

bool router_is_local_active(const device_state_t *state) {
    return state->active_output == state->board_role;
}

router_reconcile_action_t router_reconcile_output_decision(board_role_t local_role,
                                                           uint8_t local_output,
                                                           uint32_t local_gen,
                                                           uint8_t peer_output,
                                                           uint32_t peer_gen,
                                                           bool peer_just_online) {
    if (peer_output > OUTPUT_B || local_output > OUTPUT_B) {
        return ROUTER_RECONCILE_NONE;
    }

    if (peer_gen > local_gen) {
        return ROUTER_RECONCILE_ADOPT_PEER;
    }

    if (peer_gen < local_gen) {
        return peer_just_online ? ROUTER_RECONCILE_BROADCAST : ROUTER_RECONCILE_NONE;
    }

    if (peer_output != local_output) {
        if (local_role == BOARD_ROLE_B) {
            return ROUTER_RECONCILE_ADOPT_PEER;
        }
        if (local_role == BOARD_ROLE_A) {
            return ROUTER_RECONCILE_BROADCAST;
        }
        return ROUTER_RECONCILE_NONE;
    }

    if (peer_just_online) {
        return ROUTER_RECONCILE_BROADCAST;
    }

    return ROUTER_RECONCILE_NONE;
}

/*
 * Build the eight-byte body of MSG_SELECT_OUTPUT.
 *
 * Purpose:
 *   Carry one output-selection decision and its generation between boards, or
 *   create the same payload locally before passing it to router_on_select_output().
 *   The generation lets the receiver reject a stale selection packet.
 *
 * Input:
 *   payload  caller-provided PACKET_PAYLOAD_LEN-byte output buffer.
 *   output   requested active output: OUTPUT_A or OUTPUT_B.
 *   gen      version of that output-selection decision.
 *
 * Output layout:
 *   [0] output, [1..4] generation (little-endian), [5] switch reason, [6..7] zero.
 */
static void pack_select_payload(uint8_t payload[PACKET_PAYLOAD_LEN],
                                uint8_t output,
                                uint32_t gen,
                                uint8_t reason) {
    memset(payload, 0, PACKET_PAYLOAD_LEN);
    /* Initialize reserved bytes to zero before writing the meaningful fields. */

    payload[0] = output;
    /* The output the receiving router should make active. */

    payload[1] = (uint8_t)(gen & 0xFF);
    /* Generation byte 0: least-significant byte. */

    payload[2] = (uint8_t)((gen >> 8) & 0xFF);
    /* Generation byte 1. */

    payload[3] = (uint8_t)((gen >> 16) & 0xFF);
    /* Generation byte 2. */

    payload[4] = (uint8_t)((gen >> 24) & 0xFF);
    /* Generation byte 3: most-significant byte, completing little-endian. */

    payload[5] = reason;
}

/*
 * Decode the little-endian generation stored in a MSG_SELECT_OUTPUT payload.
 *
 * Purpose:
 *   Recover the monotonic version number used to decide whether a peer's
 *   output-selection packet is newer than the local decision.
 *
 * Input:
 *   payload  a valid SELECT payload whose bytes [1..4] contain generation.
 *
 * Output:
 *   Returns the 32-bit generation reconstructed from bytes [1] (least
 *   significant) through [4] (most significant).  It does not validate data.
 */
static uint32_t unpack_gen(const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    return (uint32_t)payload[1] |
           /* Generation byte 0 remains in bits 0..7 after promotion. */
           ((uint32_t)payload[2] << 8) |
           /* Place generation byte 1 in bits 8..15. */
           ((uint32_t)payload[3] << 16) |
           /* Place generation byte 2 in bits 16..23. */
           ((uint32_t)payload[4] << 24);
           /* Place generation byte 3 in bits 24..31, completing little-endian. */
}

/*
 * Queue the current output-selection decision for transmission to the peer.
 *
 * Purpose:
 *   Send active_output, its generation, and the reason it changed as one
 *   MSG_SELECT_OUTPUT packet.  The UART layer later adds frame preamble and
 *   checksum around this payload.
 *
 * Input:
 *   state   non-NULL local state containing the decision already committed.
 *   reason  SWITCH_REASON_HOTKEY, SWITCH_REASON_EDGE, or NONE for a replay.
 *
 * Output:
 *   Adds one MSG_SELECT_OUTPUT packet to the UART transmit queue.  It does not
 *   change state; callers must update active_output/generation first.
 */
static void broadcast_select(device_state_t *state, switch_reason_t reason) {
    uint8_t payload[PACKET_PAYLOAD_LEN];
    /* Temporary eight-byte body of the outgoing SELECT packet. */

    pack_select_payload(payload,
                        state->active_output,
                        state->output_generation,
                        (uint8_t)reason);
    /* Serialize the already-committed output decision and its provenance. */

    uart_queue_packet(MSG_SELECT_OUTPUT, payload);
    /* Queue only the message body; UART framing and checksum are added later. */
}

/*
 * Queue the current local output-selection state for delivery to the peer as
 * MSG_SELECT_OUTPUT.
 *
 * Purpose:
 *   Synchronize a peer after a local switch, reconnect, or heartbeat-based
 *   conflict recovery.  The peer applies the payload through
 *   router_on_select_output().
 *
 * Input:
 *   state      supplies the already-decided active_output and its generation.
 *
 * Output:
 *   A complete MSG_SELECT_OUTPUT packet is queued for UART transmission.
 *   This function does not change active_output or increment generation; its
 *   caller must make that decision before broadcasting it.
 */
void router_broadcast_active_output(device_state_t *state) {
    broadcast_select(state, SWITCH_REASON_NONE);
}

/*
 * Release all input state owned by this board that is currently applied to an
 * output which is about to lose control.
 *
 * Purpose:
 *   Ensure the old PC receives keyboard and mouse-button releases before an
 *   output switch changes routing, preventing stuck keys and stuck buttons.
 *
 * Input:
 *   state   identifies the executing board and supplies pointer state.
 *   output  the old active output to release.
 *
 * Output:
 *   Queues release reports locally when this board owns the old PC, or sends
 *   only the input type owned by this board over UART when the old PC is remote.
 *   Keyboard originates on A; mouse originates on B.
 */
void router_release_output(device_state_t *state, uint8_t output) {
    hid_keyboard_report_t empty_kbd = {0};
    /* An all-zero keyboard report releases every key. */

    mouse_abs_report_t empty_mouse;
    /* Filled only when board B must send a remote mouse-button release. */

    if (output == state->board_role) {
        /* The old PC is attached to this board, so release both local HID paths. */
        keyboard_queue_local(&empty_kbd);
        mouse_release_local();
        /* No UART release is needed because this board owns the old output. */
        return;
    }

    if (state->board_role == ROLE_A && output == OUTPUT_B) {
        /* PC B is remote from A; A owns keyboard input, so release it over UART. */
        uart_queue_keyboard(&empty_kbd);
    }

    if (state->board_role == ROLE_B && output == OUTPUT_A) {
        /* PC A is remote from B; B owns mouse input, so release it over UART. */
        mouse_build_report(state, 0, 0, &empty_mouse);
        /* Keep the current absolute position but clear all mouse buttons and wheel. */

        uart_queue_mouse(&empty_mouse);
    }
}

/*
 * Commit an output-selection decision after the old output is already safe.
 *
 * Purpose:
 *   Apply the new routing target locally, and when this board originated the
 *   decision, version it and advertise the same decision to the peer.
 *
 * Input:
 *   state        non-NULL local shared state.  The caller has already released
 *                old-output input if an actual switch was needed.
 *   new_output   destination: OUTPUT_A or OUTPUT_B.
 *   notify_peer  true for a locally owned decision that must be broadcast.
 *   reason       origin metadata carried in MSG_SELECT_OUTPUT.
 *
 * Output:
 *   On a valid destination, stores active_output.  With notify_peer true,
 *   increments output_generation and queues MSG_SELECT_OUTPUT.  It never sends
 *   release reports itself; router_release_output() is deliberately separate.
 */
void router_commit_output(device_state_t *state,
                          uint8_t new_output,
                          bool notify_peer,
                          switch_reason_t reason) {
    if (new_output > OUTPUT_B) {
        /* Reject malformed output values without changing state or notifying peer. */
        return;
    }

    state->active_output = new_output;
    /* From this point, subsequent keyboard/mouse reports route to new_output. */

    if (notify_peer) {
        /* Make this locally originated decision newer than all earlier decisions. */
        state->output_generation++;

        broadcast_select(state, reason);
        /* Send the same output, new generation, and switch reason to the peer. */
    }
}

/*
 * Make a locally initiated output-selection decision.
 *
 * Purpose:
 *   Safely move keyboard and mouse routing from the current PC to new_output.
 *   Input releases are queued before active_output changes, so they cannot be
 *   accidentally routed to the newly selected PC.
 *
 * Input:
 *   state        local shared device state.
 *   new_output   requested destination: OUTPUT_A or OUTPUT_B.
 *   notify_peer  true when this board owns the decision and must advertise it.
 *   reason       carried in SELECT so the mouse owner can place the pointer.
 *
 * Output:
 *   Updates active_output after releasing the old output.  With notify_peer,
 *   increments output_generation and queues MSG_SELECT_OUTPUT for the peer.
 *   Passing false performs only the local state change and sends nothing.
 */
void router_set_active_output(device_state_t *state,
                              uint8_t new_output,
                              bool notify_peer,
                              switch_reason_t reason) {
    if (new_output > OUTPUT_B) {
        /* Reject values outside the two valid outputs before touching state. */
        return;
    }

    uint8_t old = state->active_output;
    /* Preserve the current output so its input state can be released first. */

    if (old != new_output) {
        /* A real switch is required: release reports must use the old route. */
        router_release_output(state, old);
        router_commit_output(state, new_output, notify_peer, reason);
        return;
    }

    if (notify_peer) {
        /* Same output: still advance generation and rebroadcast when requested. */
        state->output_generation++;
        broadcast_select(state, reason);
    }
}

/*
 * Apply an output-selection decision received from the peer.
 *
 * Purpose:
 *   Keep both boards' active_output and output_generation synchronized after
 *   a peer switch, reconnect, or heartbeat-based recovery.  This is the
 *   receiving counterpart to router_set_active_output().
 *
 * Input:
 *   state    local shared device state to synchronize.
 *   payload  MSG_SELECT_OUTPUT body: [0] output, [1..4] generation LE,
 *            [5] switch reason, and reserved zeros thereafter.
 *
 * Output:
 *   Rejects an invalid output or stale generation.  Otherwise releases local
 *   input if the old output belongs to this board, then updates active_output
 *   and output_generation.  On board B, a HOTKEY reason places the pointer
 *   at center and arms edge switching.  It does not increment generation.
 */
void router_on_select_output(device_state_t *state, const uint8_t payload[8]) {
    uint8_t new_output = payload[0];
    /* Read the output peer selected: OUTPUT_A or OUTPUT_B. */

    if (!state->routing_enabled) {
        return;
    }

    uint32_t gen = unpack_gen(payload);
    /* Decode the selection decision's little-endian generation. */

    uint8_t reason = payload[5];

    if (new_output > OUTPUT_B) {
        /* Reject malformed output values before modifying local state. */
        return;
    }

    if (gen < state->output_generation) {
        /* A delayed packet must not overwrite the newer local decision. */
        return;
    }

    uint8_t old = state->active_output;
    /* Keep the old value to decide whether this board must release its PC. */

    if (old != new_output && old == state->board_role) {
        /* The output is changing away from the PC attached to this board. */
        hid_keyboard_report_t empty_kbd = {0};
        /* All-zero report releases any locally held keyboard keys. */

        keyboard_queue_local(&empty_kbd);

        mouse_release_local();
        /* Release both local HID paths before routing switches elsewhere. */
    }

    state->active_output = new_output;
    /* Adopt the peer's selected destination for subsequent routing. */

    state->output_generation = gen;
    /* Record the generation that authorized this state update. */

    if (state->board_role == ROLE_B &&
        old != new_output &&
        reason == (uint8_t)SWITCH_REASON_HOTKEY) {
        mouse_on_hotkey_switch(state);
    }
}

/*
 * Receive the keyboard report sent from board A over UART and, when PC B is
 * active, forward it to board B's local USB device.
 *
 * Purpose:
 *   Board A owns the physical keyboard.  This function is the board-B side of
 *   the keyboard path when routing selects PC B.
 *
 * Input:
 *   state    local shared device state.
 *   payload  MSG_KEYBOARD_REPORT body, whose bytes match hid_keyboard_report_t.
 *
 * Output:
 *   Always records the latest peer keyboard state in remote_keyboard.  Queues
 *   that report to the local USB keyboard only on board B while OUTPUT_B is
 *   active; otherwise it produces no USB output.
 */
void router_on_remote_keyboard(device_state_t *state, const uint8_t payload[8]) {
    hid_keyboard_report_t report;
    /* Local representation of the fixed-size keyboard payload. */

    if (!state->routing_enabled) {
        return;
    }

    memcpy(&report, payload, sizeof(report));
    /* Copy the payload bytes into the HID keyboard-report layout. */

    state->remote_keyboard = report;
    /* Preserve the most recent keyboard state received from board A. */

    if (state->board_role == ROLE_B && state->active_output == OUTPUT_B) {
        /* PC B is active and attached locally: forward the report over USB. */
        keyboard_queue_local(&report);
    }
}

/*
 * Receive an absolute mouse report from board B and, while PC A is active,
 * forward it to board A's local USB device.
 *
 * Purpose:
 *   Board B owns the physical mouse and its pointer accumulator.  This
 *   function is the board-A side of that mouse path when routing selects PC A.
 *
 * Input:
 *   state    local shared device state.
 *   payload  MSG_MOUSE_REPORT v2 body: buttons, absolute X/Y, wheel, and
 *            two zero reserved bytes.
 *
 * Output:
 *   Invalid payloads produce no effect.  Board A records a mirror of the
 *   received button and position state, then queues the report for local USB
 *   only while OUTPUT_A is active.  Board B ignores received mouse packets to
 *   avoid routing the same physical-mouse input twice.
 */
void router_on_remote_mouse(device_state_t *state, const uint8_t payload[8]) {
    mouse_abs_report_t report;
    /* Decoded absolute mouse report, populated only after protocol validation. */

    if (!state->routing_enabled) {
        return;
    }

    if (!protocol_unpack_mouse(payload, &report)) {
        /* Reject bad reserved bytes or coordinates outside the absolute range. */
        return;
    }

    if (state->board_role == ROLE_A) {
        /* Only board A consumes mouse reports received from board B. */
        state->mouse_buttons = report.buttons;
        /* Keep a mirror of the latest button state for release handling. */

        state->pointer_x = (int32_t)report.x;
        state->pointer_y = (int32_t)report.y;
        /* Mirror B's already-accumulated absolute pointer position; do not recalculate it. */

        if (state->active_output == OUTPUT_A) {
            /* PC A is currently selected and is attached to this board. */
            mouse_queue_local(&report);
        }
    }
}

/*
 * Fail safely after UART timeout declares the peer offline.
 *
 * Purpose:
 *   Release input state that normally originates on the missing peer, so its
 *   last keyboard keys or mouse buttons cannot remain stuck on the local PC.
 *
 * Input:
 *   state  local shared state.  The UART timeout layer has already marked the
 *          peer offline and reset its parser before calling this function.
 *
 * Output:
 *   Clears the cached remote keyboard state.  Board B queues an all-key-up
 *   report because keyboard originates on A; board A clears and releases mouse
 *   buttons because mouse originates on B.  It does not change output routing.
 */
void router_on_peer_offline(device_state_t *state) {
    hid_keyboard_report_t empty_kbd = {0};
    /* All-zero keyboard report releases every key. */

    memset(&state->remote_keyboard, 0, sizeof(state->remote_keyboard));
    /* Do not retain keyboard state received before the peer disappeared. */

    if (state->board_role == ROLE_B) {
        /* Board B loses board A, which is the keyboard-input owner. */
        keyboard_queue_local(&empty_kbd);
        /* Release any A-originated keyboard state currently applied to PC B. */
    }

    if (state->board_role == ROLE_A) {
        /* Board A loses board B, which is the mouse-input owner. */
        state->mouse_buttons = 0;
        /* Clear the local mirror before producing the USB mouse release. */

        mouse_release_local();
        /* Release any B-originated mouse buttons currently applied to PC A. */
    }
}

void router_enter_role_conflict(device_state_t *state) {
    hid_keyboard_report_t empty_kbd = {0};

    if (state->board_role == BOARD_ROLE_CONFLICT) {
        return;
    }

    state->board_role = BOARD_ROLE_CONFLICT;
    state->routing_enabled = false;
    state->peer_protocol_ok = false;
    state->peer_role_validated = false;
    state->protocol_mismatch = false;

    memset(&state->local_keyboard, 0, sizeof(state->local_keyboard));
    memset(&state->remote_keyboard, 0, sizeof(state->remote_keyboard));
    keyboard_queue_local(&empty_kbd);

    state->mouse_buttons = 0;
    mouse_release_local();
}

/*
 * Process a peer heartbeat, verify protocol compatibility, and reconcile the
 * two boards' output-selection state.
 *
 * Purpose:
 *   Heartbeats carry the peer's active output, generation, and protocol
 *   version.  This handler prevents incompatible mouse traffic and converges
 *   both boards after a missed switch, reboot, reconnect, or state conflict.
 *
 * Input:
 *   state             local shared device state.
 *   payload           heartbeat body: role, active output, generation, and
 *                     protocol version.
 *   peer_just_online  true only for the first packet after peer reconnect.
 *
 * Output:
 *   Updates peer protocol flags and may apply the peer's selection or queue a
 *   local MSG_SELECT_OUTPUT.  Generation comparison wins first; if generation
 *   ties but outputs differ, board A is the deterministic authority.
 */
void router_on_peer_heartbeat(device_state_t *state, const uint8_t payload[8], bool peer_just_online) {
    uint8_t peer_role_raw = 0;
    uint8_t peer_output = 0;
    uint32_t peer_gen = 0;
    uint8_t peer_version = 0;

    if (!protocol_unpack_heartbeat(payload, &peer_role_raw, &peer_output, &peer_gen, &peer_version)) {
        return;
    }

    state->peer_protocol_version = peer_version;

    if (peer_version != DESKHOP_PROTOCOL_VERSION) {
        state->peer_protocol_ok = false;
        state->peer_role_validated = false;
        state->protocol_mismatch = true;
        return;
    }

    state->protocol_mismatch = false;

    if ((board_role_t)peer_role_raw == BOARD_ROLE_CONFLICT) {
        state->peer_role = BOARD_ROLE_CONFLICT;
        router_enter_role_conflict(state);
        return;
    }

    if (!board_role_is_concrete((board_role_t)peer_role_raw)) {
        state->peer_protocol_ok = false;
        state->peer_role_validated = false;
        return;
    }

    state->peer_role = (board_role_t)peer_role_raw;

    if (state->board_role == BOARD_ROLE_CONFLICT) {
        state->peer_protocol_ok = false;
        state->peer_role_validated = false;
        return;
    }

    if (!board_role_is_peer_of(state->board_role, state->peer_role)) {
        router_enter_role_conflict(state);
        return;
    }

    state->peer_protocol_ok = true;
    state->peer_role_validated = true;

    if (peer_output > OUTPUT_B) {
        return;
    }

    router_reconcile_action_t action =
        router_reconcile_output_decision(state->board_role,
                                         state->active_output,
                                         state->output_generation,
                                         peer_output,
                                         peer_gen,
                                         peer_just_online);

    if (action == ROUTER_RECONCILE_ADOPT_PEER) {
        uint8_t sel[PACKET_PAYLOAD_LEN];
        pack_select_payload(sel, peer_output, peer_gen, SWITCH_REASON_NONE);
        router_on_select_output(state, sel);
        return;
    }

    if (action == ROUTER_RECONCILE_BROADCAST) {
        router_broadcast_active_output(state);
    }
}

bool router_output_selftest(void) {
    if (DEFAULT_ACTIVE_OUTPUT != OUTPUT_A) {
        return false;
    }

    /* Bootstrap defaults both boards claim after board_init. */
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_A, 0,
                                         OUTPUT_A, 0, true) !=
        ROUTER_RECONCILE_BROADCAST) {
        return false;
    }
    if (router_reconcile_output_decision(BOARD_ROLE_B, OUTPUT_A, 0,
                                         OUTPUT_A, 0, true) !=
        ROUTER_RECONCILE_BROADCAST) {
        return false;
    }

    /* B converges on equal-gen output mismatch; A refuses. */
    if (router_reconcile_output_decision(BOARD_ROLE_B, OUTPUT_B, 0,
                                         OUTPUT_A, 0, false) !=
        ROUTER_RECONCILE_ADOPT_PEER) {
        return false;
    }
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_A, 0,
                                         OUTPUT_B, 0, false) !=
        ROUTER_RECONCILE_BROADCAST) {
        return false;
    }

    /* Newer generation wins regardless of which board rebooted first. */
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_A, 0,
                                         OUTPUT_B, 5, false) !=
        ROUTER_RECONCILE_ADOPT_PEER) {
        return false;
    }
    if (router_reconcile_output_decision(BOARD_ROLE_B, OUTPUT_A, 0,
                                         OUTPUT_B, 3, false) !=
        ROUTER_RECONCILE_ADOPT_PEER) {
        return false;
    }

    /* Local newer: reassert only on peer_just_online. */
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_B, 4,
                                         OUTPUT_A, 1, true) !=
        ROUTER_RECONCILE_BROADCAST) {
        return false;
    }
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_B, 4,
                                         OUTPUT_A, 1, false) !=
        ROUTER_RECONCILE_NONE) {
        return false;
    }

    /* Steady state equal gen / same output. */
    if (router_reconcile_output_decision(BOARD_ROLE_A, OUTPUT_B, 2,
                                         OUTPUT_B, 2, false) !=
        ROUTER_RECONCILE_NONE) {
        return false;
    }

    return true;
}
