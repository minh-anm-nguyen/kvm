#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    uint8_t board_role;    /* ROLE_A or ROLE_B (from BOARD_ROLE) */
    uint8_t active_output; /* OUTPUT_A or OUTPUT_B               */
    bool    peer_online;
    bool    usb_device_ready;
    bool    input_connected;

    uint8_t  mouse_buttons;
    uint32_t last_peer_heartbeat_ms;
    uint32_t output_generation;

    bool led_on;
} device_state_t;

extern device_state_t g_state;
