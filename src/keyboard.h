#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "class/hid/hid.h"
#include "state.h"

void keyboard_init(void);
void keyboard_on_report(const uint8_t *raw, uint16_t len);
void keyboard_on_unmount(void);
void keyboard_task(device_state_t *state);
