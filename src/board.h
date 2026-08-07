#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

typedef struct {
    uint8_t uart_rx;
    uint8_t uart_tx;
    uint8_t usb_host_dp;
} board_pinmap_t;

typedef enum {
    PROBE_ROLE_A = 0,
    PROBE_ROLE_B,
    PROBE_AMBIGUOUS,
} role_probe_result_t;

bool board_role_is_concrete(board_role_t role);
bool board_role_is_peer_of(board_role_t local, board_role_t peer);
bool board_accepts_keyboard(board_role_t role);
bool board_accepts_mouse(board_role_t role);
const board_pinmap_t *board_get_pinmap(board_role_t role);
bool board_pinmap_selftest(void);
bool board_ownership_selftest(void);

role_probe_result_t board_probe_role_once(void);
bool board_probe_selftest(void);

board_role_t board_detect_role(void);
bool board_detect_selftest(void);
#ifdef KVM_DEBUG
unsigned board_probe_last_attempts(void);
#endif

void board_init(device_state_t *state);
void board_boot_resolve_role(device_state_t *state);
void board_note_wrong_port_input(void);
void board_enable_watchdog(void);
void board_update_led(device_state_t *state);
void board_kick_watchdog(void);

uint32_t board_millis(void);
