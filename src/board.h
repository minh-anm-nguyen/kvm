#pragma once

#include "state.h"

void board_init(device_state_t *state);
void board_enable_watchdog(void);
void board_update_led(device_state_t *state);
void board_kick_watchdog(void);

uint32_t board_millis(void);
