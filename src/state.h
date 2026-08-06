#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "class/hid/hid.h"
#include "config.h"

typedef struct {
    uint8_t board_role;
    uint8_t active_output;
    bool    peer_online;
    bool    usb_device_ready;
    bool    input_connected;
    bool    mouse_connected;

    uint8_t  kbd_dev_addr;
    uint8_t  kbd_instance;
    uint8_t  mouse_dev_addr;
    uint8_t  mouse_instance;
    hid_keyboard_report_t local_keyboard;
    hid_keyboard_report_t remote_keyboard;

    uint8_t  mouse_buttons;
    int32_t  pointer_x;
    int32_t  pointer_y;
    bool     edge_switch_armed;
    uint32_t last_peer_heartbeat_ms;
    uint32_t output_generation;

    bool led_on;
} device_state_t;

extern device_state_t g_state;
