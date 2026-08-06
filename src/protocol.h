#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "mouse.h"

enum message_type {
    MSG_KEYBOARD_REPORT = 1,
    MSG_MOUSE_REPORT    = 2,
    MSG_SELECT_OUTPUT   = 3,
    MSG_HEARTBEAT       = 4,
};

#define PACKET_PAYLOAD_LEN 8
#define PACKET_PREAMBLE_LEN 2
#define PACKET_TYPE_LEN 1
#define PACKET_CHECKSUM_LEN 1

typedef struct {
    uint8_t type;
    uint8_t payload[PACKET_PAYLOAD_LEN];
} uart_packet_t;

typedef struct {
    uint8_t buf[PACKET_SIZE];
    uint8_t len;
} protocol_parser_t;

uint8_t protocol_checksum(const uint8_t payload[PACKET_PAYLOAD_LEN]);
bool protocol_type_valid(uint8_t type);

void protocol_encode(uint8_t out[PACKET_SIZE], uint8_t type, const uint8_t payload[PACKET_PAYLOAD_LEN]);
bool protocol_decode(const uint8_t in[PACKET_SIZE], uart_packet_t *out);

void protocol_parser_reset(protocol_parser_t *parser);
bool protocol_parser_feed(protocol_parser_t *parser, uint8_t byte, uart_packet_t *out);

bool protocol_pack_mouse(const mouse_abs_report_t *report, uint8_t payload[PACKET_PAYLOAD_LEN]);
bool protocol_unpack_mouse(const uint8_t payload[PACKET_PAYLOAD_LEN], mouse_abs_report_t *report);

void protocol_pack_heartbeat(uint8_t payload[PACKET_PAYLOAD_LEN],
                             uint8_t role,
                             uint8_t active_output,
                             uint32_t generation);
bool protocol_unpack_heartbeat(const uint8_t payload[PACKET_PAYLOAD_LEN],
                               uint8_t *role,
                               uint8_t *active_output,
                               uint32_t *generation,
                               uint8_t *version);

bool protocol_selftest(void);
