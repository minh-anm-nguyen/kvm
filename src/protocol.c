#include "protocol.h"

#include <string.h>

uint8_t protocol_checksum(const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    uint8_t checksum = 0;
    for (int i = 0; i < PACKET_PAYLOAD_LEN; i++) {
        checksum ^= payload[i];
    }
    return checksum;
}

bool protocol_type_valid(uint8_t type) {
    return type == MSG_KEYBOARD_REPORT ||
           type == MSG_MOUSE_REPORT ||
           type == MSG_SELECT_OUTPUT ||
           type == MSG_HEARTBEAT;
}

void protocol_encode(uint8_t out[PACKET_SIZE], uint8_t type, const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    out[0] = PACKET_PREAMBLE_0;
    out[1] = PACKET_PREAMBLE_1;
    out[2] = type;
    memcpy(&out[3], payload, PACKET_PAYLOAD_LEN);
    out[11] = protocol_checksum(payload);
}

bool protocol_decode(const uint8_t in[PACKET_SIZE], uart_packet_t *out) {
    if (in[0] != PACKET_PREAMBLE_0 || in[1] != PACKET_PREAMBLE_1) {
        return false;
    }
    if (!protocol_type_valid(in[2])) {
        return false;
    }
    if (protocol_checksum(&in[3]) != in[11]) {
        return false;
    }

    out->type = in[2];
    memcpy(out->payload, &in[3], PACKET_PAYLOAD_LEN);
    return true;
}

void protocol_parser_reset(protocol_parser_t *parser) {
    parser->len = 0;
}

static bool protocol_parser_resync(protocol_parser_t *parser, uart_packet_t *out) {
    uint8_t snapshot[PACKET_SIZE];
    uint8_t n = parser->len;

    if (n <= 1) {
        protocol_parser_reset(parser);
        return false;
    }

    memcpy(snapshot, parser->buf, n);
    protocol_parser_reset(parser);

    for (uint8_t i = 1; i < n; i++) {
        if (protocol_parser_feed(parser, snapshot[i], out)) {
            return true;
        }
    }
    return false;
}

bool protocol_parser_feed(protocol_parser_t *parser, uint8_t byte, uart_packet_t *out) {
    if (parser->len == 0) {
        if (byte == PACKET_PREAMBLE_0) {
            parser->buf[0] = byte;
            parser->len = 1;
        }
        return false;
    }

    if (parser->len == 1) {
        if (byte == PACKET_PREAMBLE_1) {
            parser->buf[1] = byte;
            parser->len = 2;
        } else if (byte == PACKET_PREAMBLE_0) {
            parser->buf[0] = byte;
            parser->len = 1;
        } else {
            protocol_parser_reset(parser);
        }
        return false;
    }

    parser->buf[parser->len++] = byte;
    if (parser->len < PACKET_SIZE) {
        return false;
    }

    if (protocol_decode(parser->buf, out)) {
        protocol_parser_reset(parser);
        return true;
    }

    return protocol_parser_resync(parser, out);
}
