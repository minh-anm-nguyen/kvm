#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "class/hid/hid.h"
#include "state.h"

#define ITF_NUM_KEYBOARD 0
#define ITF_NUM_MOUSE    1
#define ITF_NUM_TOTAL    2

void usb_device_init(void);
void usb_device_task(device_state_t *state);

bool usb_device_ready(void);
bool usb_device_send_keyboard(const hid_keyboard_report_t *report);
bool usb_device_send_keyboard_empty(void);
bool usb_device_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
