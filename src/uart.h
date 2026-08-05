#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "state.h"

void uart_link_init(uint8_t board_role);
void uart_link_task(device_state_t *state);

void uart_write_bytes(const uint8_t *data, size_t len);
bool uart_queue_packet(uint8_t type, const uint8_t payload[PACKET_PAYLOAD_LEN]);
bool uart_queue_heartbeat(const device_state_t *state);
