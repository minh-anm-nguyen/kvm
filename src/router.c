#include "router.h"

#include <string.h>

#include "keyboard.h"
#include "mouse.h"
#include "uart.h"

bool router_is_local_active(const device_state_t *state) {
    return state->active_output == state->board_role;
}

static void pack_select_payload(uint8_t payload[PACKET_PAYLOAD_LEN], uint8_t output, uint32_t gen) {
    memset(payload, 0, PACKET_PAYLOAD_LEN);
    payload[0] = output;
    payload[1] = (uint8_t)(gen & 0xFF);
    payload[2] = (uint8_t)((gen >> 8) & 0xFF);
    payload[3] = (uint8_t)((gen >> 16) & 0xFF);
    payload[4] = (uint8_t)((gen >> 24) & 0xFF);
}

static uint32_t unpack_gen(const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    return (uint32_t)payload[1] |
           ((uint32_t)payload[2] << 8) |
           ((uint32_t)payload[3] << 16) |
           ((uint32_t)payload[4] << 24);
}

void router_broadcast_active_output(device_state_t *state) {
    uint8_t payload[PACKET_PAYLOAD_LEN];
    pack_select_payload(payload, state->active_output, state->output_generation);
    uart_queue_packet(MSG_SELECT_OUTPUT, payload);
}

static void release_output(device_state_t *state, uint8_t output) {
    hid_keyboard_report_t empty_kbd = {0};
    mouse_abs_report_t empty_mouse;

    if (output == state->board_role) {
        keyboard_queue_local(&empty_kbd);
        mouse_release_local();
        return;
    }

    if (state->board_role == ROLE_A && output == OUTPUT_B) {
        uart_queue_keyboard(&empty_kbd);
    }

    if (state->board_role == ROLE_B && output == OUTPUT_A) {
        mouse_build_report(state, 0, 0, &empty_mouse);
        uart_queue_mouse(&empty_mouse);
    }
}

void router_set_active_output(device_state_t *state, uint8_t new_output, bool notify_peer) {
    if (new_output > OUTPUT_B) {
        return;
    }

    uint8_t old = state->active_output;

    if (old != new_output) {
        release_output(state, old);
        state->active_output = new_output;
    }

    if (notify_peer) {
        state->output_generation++;
        router_broadcast_active_output(state);
    }
}

void router_on_select_output(device_state_t *state, const uint8_t payload[8]) {
    uint8_t new_output = payload[0];
    uint32_t gen = unpack_gen(payload);

    if (new_output > OUTPUT_B) {
        return;
    }

    if (gen < state->output_generation) {
        return;
    }

    uint8_t old = state->active_output;
    if (old != new_output && old == state->board_role) {
        hid_keyboard_report_t empty_kbd = {0};
        keyboard_queue_local(&empty_kbd);
        mouse_release_local();
    }

    state->active_output = new_output;
    state->output_generation = gen;
}

void router_on_remote_keyboard(device_state_t *state, const uint8_t payload[8]) {
    hid_keyboard_report_t report;
    memcpy(&report, payload, sizeof(report));
    state->remote_keyboard = report;

    if (state->board_role == ROLE_B && state->active_output == OUTPUT_B) {
        keyboard_queue_local(&report);
    }
}

void router_on_remote_mouse(device_state_t *state, const uint8_t payload[8]) {
    mouse_abs_report_t report;

    memset(&report, 0, sizeof(report));
    report.buttons = payload[0];
    report.x = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    report.y = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
    report.wheel = (int8_t)payload[5];

    if (report.x > (uint16_t)POINTER_MAX) {
        report.x = (uint16_t)POINTER_MAX;
    }
    if (report.y > (uint16_t)POINTER_MAX) {
        report.y = (uint16_t)POINTER_MAX;
    }

    if (state->board_role == ROLE_A) {
        state->mouse_buttons = report.buttons;
        state->pointer_x = (int32_t)report.x;
        state->pointer_y = (int32_t)report.y;
        if (state->active_output == OUTPUT_A) {
            mouse_queue_local(&report);
        }
    }
}

void router_on_peer_offline(device_state_t *state) {
    hid_keyboard_report_t empty_kbd = {0};
    memset(&state->remote_keyboard, 0, sizeof(state->remote_keyboard));

    if (state->board_role == ROLE_B) {
        keyboard_queue_local(&empty_kbd);
    }

    if (state->board_role == ROLE_A) {
        state->mouse_buttons = 0;
        mouse_release_local();
    }
}

void router_on_peer_heartbeat(device_state_t *state, const uint8_t payload[8], bool peer_just_online) {
    uint8_t peer_output = payload[1];
    uint32_t peer_gen = (uint32_t)payload[2] |
                        ((uint32_t)payload[3] << 8) |
                        ((uint32_t)payload[4] << 16) |
                        ((uint32_t)payload[5] << 24);

    if (peer_output > OUTPUT_B) {
        return;
    }

    if (peer_gen > state->output_generation) {
        uint8_t sel[PACKET_PAYLOAD_LEN];
        pack_select_payload(sel, peer_output, peer_gen);
        router_on_select_output(state, sel);
        return;
    }

    if (peer_gen < state->output_generation) {
        if (peer_just_online) {
            router_broadcast_active_output(state);
        }
        return;
    }

    if (peer_output != state->active_output) {
        if (state->board_role == ROLE_B) {
            uint8_t sel[PACKET_PAYLOAD_LEN];
            pack_select_payload(sel, peer_output, peer_gen);
            router_on_select_output(state, sel);
        } else {
            router_broadcast_active_output(state);
        }
        return;
    }

    if (peer_just_online) {
        router_broadcast_active_output(state);
    }
}
