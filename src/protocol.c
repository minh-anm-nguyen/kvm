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

bool protocol_pack_mouse(const mouse_abs_report_t *report, uint8_t payload[PACKET_PAYLOAD_LEN]) {
    if (report == NULL || payload == NULL) {
        return false;
    }
    if (report->x > (uint16_t)POINTER_MAX || report->y > (uint16_t)POINTER_MAX) {
        return false;
    }

    memset(payload, 0, PACKET_PAYLOAD_LEN);
    payload[0] = report->buttons;
    payload[1] = (uint8_t)(report->x & 0xFF);
    payload[2] = (uint8_t)((report->x >> 8) & 0xFF);
    payload[3] = (uint8_t)(report->y & 0xFF);
    payload[4] = (uint8_t)((report->y >> 8) & 0xFF);
    payload[5] = (uint8_t)report->wheel;
    payload[6] = 0;
    payload[7] = 0;
    return true;
}

bool protocol_unpack_mouse(const uint8_t payload[PACKET_PAYLOAD_LEN], mouse_abs_report_t *report) {
    if (payload == NULL || report == NULL) {
        return false;
    }
    if (payload[6] != 0 || payload[7] != 0) {
        return false;
    }

    uint16_t x = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    uint16_t y = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
    if (x > (uint16_t)POINTER_MAX || y > (uint16_t)POINTER_MAX) {
        return false;
    }

    memset(report, 0, sizeof(*report));
    report->buttons = payload[0];
    report->x = x;
    report->y = y;
    report->wheel = (int8_t)payload[5];
    return true;
}

void protocol_pack_heartbeat(uint8_t payload[PACKET_PAYLOAD_LEN],
                             uint8_t role,
                             uint8_t active_output,
                             uint32_t generation) {
    memset(payload, 0, PACKET_PAYLOAD_LEN);
    payload[0] = role;
    payload[1] = active_output;
    payload[2] = (uint8_t)(generation & 0xFF);
    payload[3] = (uint8_t)((generation >> 8) & 0xFF);
    payload[4] = (uint8_t)((generation >> 16) & 0xFF);
    payload[5] = (uint8_t)((generation >> 24) & 0xFF);
    payload[6] = (uint8_t)DESKHOP_PROTOCOL_VERSION;
    payload[7] = 0;
}

bool protocol_unpack_heartbeat(const uint8_t payload[PACKET_PAYLOAD_LEN],
                               uint8_t *role,
                               uint8_t *active_output,
                               uint32_t *generation,
                               uint8_t *version) {
    if (payload == NULL || role == NULL || active_output == NULL ||
        generation == NULL || version == NULL) {
        return false;
    }

    *role = payload[0];
    *active_output = payload[1];
    *generation = (uint32_t)payload[2] |
                  ((uint32_t)payload[3] << 8) |
                  ((uint32_t)payload[4] << 16) |
                  ((uint32_t)payload[5] << 24);
    *version = payload[6];
    return true;
}

bool protocol_selftest(void) {
    /* Golden absolute mouse payload */
    mouse_abs_report_t report = {
        .buttons = 0x05,
        .x = 0x1234,
        .y = 0x5678,
        .wheel = -3,
        .reserved = {0, 0},
    };
    uint8_t payload[PACKET_PAYLOAD_LEN];
    const uint8_t golden_payload[PACKET_PAYLOAD_LEN] = {
        0x05, 0x34, 0x12, 0x78, 0x56, (uint8_t)-3, 0x00, 0x00,
    };

    if (!protocol_pack_mouse(&report, payload)) {
        return false;
    }
    if (memcmp(payload, golden_payload, PACKET_PAYLOAD_LEN) != 0) {
        return false;
    }

    mouse_abs_report_t decoded = {0};
    if (!protocol_unpack_mouse(payload, &decoded)) {
        return false;
    }
    if (decoded.buttons != report.buttons ||
        decoded.x != report.x ||
        decoded.y != report.y ||
        decoded.wheel != report.wheel) {
        return false;
    }

    /* Full packet encode/decode round-trip */
    uint8_t raw[PACKET_SIZE];
    uart_packet_t pkt;
    protocol_encode(raw, MSG_MOUSE_REPORT, payload);
    if (raw[0] != PACKET_PREAMBLE_0 || raw[1] != PACKET_PREAMBLE_1 ||
        raw[2] != MSG_MOUSE_REPORT) {
        return false;
    }
    if (!protocol_decode(raw, &pkt) || pkt.type != MSG_MOUSE_REPORT) {
        return false;
    }
    if (memcmp(pkt.payload, golden_payload, PACKET_PAYLOAD_LEN) != 0) {
        return false;
    }

    /* Reject reserved non-zero */
    uint8_t bad_reserved[PACKET_PAYLOAD_LEN];
    memcpy(bad_reserved, golden_payload, PACKET_PAYLOAD_LEN);
    bad_reserved[6] = 1;
    if (protocol_unpack_mouse(bad_reserved, &decoded)) {
        return false;
    }

    /* Reject out-of-range coordinates */
    mouse_abs_report_t oversized = report;
    oversized.x = (uint16_t)(POINTER_MAX + 1);
    if (protocol_pack_mouse(&oversized, payload)) {
        return false;
    }

    uint8_t over_payload[PACKET_PAYLOAD_LEN] = {0};
    over_payload[1] = 0xFF;
    over_payload[2] = 0xFF;
    if (protocol_unpack_mouse(over_payload, &decoded)) {
        return false;
    }

    /* Heartbeat carries protocol version */
    uint8_t hb[PACKET_PAYLOAD_LEN];
    protocol_pack_heartbeat(hb, ROLE_A, OUTPUT_B, 0x01020304u);
    if (hb[0] != ROLE_A || hb[1] != OUTPUT_B ||
        hb[2] != 0x04 || hb[3] != 0x03 || hb[4] != 0x02 || hb[5] != 0x01 ||
        hb[6] != DESKHOP_PROTOCOL_VERSION || hb[7] != 0) {
        return false;
    }

    uint8_t role = 0xFF;
    uint8_t output = 0xFF;
    uint32_t gen = 0;
    uint8_t ver = 0;
    if (!protocol_unpack_heartbeat(hb, &role, &output, &gen, &ver)) {
        return false;
    }
    if (role != ROLE_A || output != OUTPUT_B || gen != 0x01020304u ||
        ver != DESKHOP_PROTOCOL_VERSION) {
        return false;
    }

    /* Center report used for default A path (UART) and local B path */
    mouse_abs_report_t center = {
        .buttons = 0,
        .x = (uint16_t)POINTER_CENTER,
        .y = (uint16_t)POINTER_CENTER,
        .wheel = 0,
        .reserved = {0, 0},
    };
    if (!protocol_pack_mouse(&center, payload) ||
        !protocol_unpack_mouse(payload, &decoded)) {
        return false;
    }
    if (decoded.x != (uint16_t)POINTER_CENTER ||
        decoded.y != (uint16_t)POINTER_CENTER) {
        return false;
    }

    return true;
}
