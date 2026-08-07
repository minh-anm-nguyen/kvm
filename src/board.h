#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

typedef struct {
    uint8_t uart_rx;
    uint8_t uart_tx;
    uint8_t usb_host_dp;
} board_pinmap_t;

bool board_role_is_concrete(board_role_t role);
const board_pinmap_t *board_get_pinmap(board_role_t role);
bool board_pinmap_selftest(void);

void board_init(device_state_t *state);
void board_enable_watchdog(void);
void board_update_led(device_state_t *state);
void board_kick_watchdog(void);

uint32_t board_millis(void);
