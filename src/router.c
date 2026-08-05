#include "router.h"

#include <string.h>

#include "keyboard.h"
#include "uart.h"

bool router_is_local_active(const device_state_t *state) {
    return state->active_output == state->board_role;
}

static void release_output(device_state_t *state, uint8_t output) {
    hid_keyboard_report_t empty = {0};

    if (output == state->board_role) {
        keyboard_queue_local(&empty);
        return;
    }

    if (state->board_role == ROLE_A && output == OUTPUT_B) {
        uart_queue_keyboard(&empty);
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

        uint8_t payload[PACKET_PAYLOAD_LEN] = {0};
        payload[0] = state->active_output;
        payload[1] = (uint8_t)(state->output_generation & 0xFF);
        payload[2] = (uint8_t)((state->output_generation >> 8) & 0xFF);
        payload[3] = (uint8_t)((state->output_generation >> 16) & 0xFF);
        payload[4] = (uint8_t)((state->output_generation >> 24) & 0xFF);
        uart_queue_packet(MSG_SELECT_OUTPUT, payload);
    }
}

void router_on_select_output(device_state_t *state, const uint8_t payload[8]) {
    uint8_t new_output = payload[0];
    uint32_t gen = (uint32_t)payload[1] |
                   ((uint32_t)payload[2] << 8) |
                   ((uint32_t)payload[3] << 16) |
                   ((uint32_t)payload[4] << 24);

    if (new_output > OUTPUT_B) {
        return;
    }

    if (gen < state->output_generation) {
        return;
    }

    uint8_t old = state->active_output;
    if (old != new_output && old == state->board_role) {
        hid_keyboard_report_t empty = {0};
        keyboard_queue_local(&empty);
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
