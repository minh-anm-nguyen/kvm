#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

void router_set_active_output(device_state_t *state, uint8_t new_output, bool notify_peer);
void router_on_select_output(device_state_t *state, const uint8_t payload[8]);
void router_on_remote_keyboard(device_state_t *state, const uint8_t payload[8]);
void router_on_remote_mouse(device_state_t *state, const uint8_t payload[8]);
void router_on_peer_heartbeat(device_state_t *state, const uint8_t payload[8], bool peer_just_online);
void router_on_peer_offline(device_state_t *state);
void router_broadcast_active_output(device_state_t *state);

bool router_is_local_active(const device_state_t *state);
