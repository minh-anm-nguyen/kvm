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

/*
 * Serialize one fixed-size UART protocol frame.
 *
 * Purpose:
 *   Add framing around an already prepared eight-byte payload so the receiver
 *   can find packet boundaries and verify that payload data was not corrupted.
 *
 * Input:
 *   out      writable PACKET_SIZE-byte destination buffer.
 *   type     message type byte selected by the caller.
 *   payload  exactly PACKET_PAYLOAD_LEN bytes to transmit.
 *
 * Output layout:
 *   [0..1] preamble, [2] type, [3..10] payload, [11] XOR checksum of payload.
 *   This helper does not validate pointers or type; callers supply valid input.
 */
void protocol_encode(uint8_t out[PACKET_SIZE], uint8_t type, const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    out[0] = PACKET_PREAMBLE_0;
    /* Write the first byte that marks the beginning of every frame. */

    out[1] = PACKET_PREAMBLE_1;
    /* Write the second preamble byte required to confirm frame alignment. */

    out[2] = type;
    /* Store the message kind immediately before its payload. */

    memcpy(&out[3], payload, PACKET_PAYLOAD_LEN);
    /* Copy all eight payload bytes without altering their wire representation. */

    out[11] = protocol_checksum(payload);
    /* Protect the payload with its XOR checksum; preamble and type are excluded. */
}

/*
 * Validate and unpack one complete UART frame.
 *
 * Purpose:
 *   Reject misaligned, unknown, or corrupted frames before any router code can
 *   act on their type and payload.
 *
 * Input:
 *   in   candidate PACKET_SIZE-byte frame.
 *   out  non-NULL destination for the decoded type and eight-byte payload.
 *
 * Output:
 *   Returns true and fills *out only when preamble, type, and payload checksum
 *   are valid.  Returns false without intentionally modifying *out otherwise.
 */
bool protocol_decode(const uint8_t in[PACKET_SIZE], uart_packet_t *out) {
    if (in[0] != PACKET_PREAMBLE_0 || in[1] != PACKET_PREAMBLE_1) {
        /* These bytes do not begin a protocol frame at the assumed boundary. */
        return false;
    }

    if (!protocol_type_valid(in[2])) {
        /* Reject unrecognized message kinds even if framing looks correct. */
        return false;
    }

    if (protocol_checksum(&in[3]) != in[11]) {
        /* A payload byte was corrupted or this was a false frame boundary. */
        return false;
    }

    out->type = in[2];
    /* Expose the validated message type to the UART receive path. */

    memcpy(out->payload, &in[3], PACKET_PAYLOAD_LEN);
    /* Copy the validated payload in its original eight-byte form. */

    return true;
}

/*
 * Forget all bytes accumulated for the current candidate UART frame.
 *
 * Input:
 *   parser  non-NULL incremental parser state.
 *
 * Output:
 *   Sets len to zero, so the next byte is treated as a possible first preamble
 *   byte.  Old bytes remain in buf but are outside len and therefore ignored.
 */
void protocol_parser_reset(protocol_parser_t *parser) {
    parser->len = 0;
    /* Resetting length is sufficient because buf contents are never read past len. */
}

/*
 * Recover parser alignment after a full candidate frame fails validation.
 *
 * Purpose:
 *   Do not discard a valid frame that may start inside the rejected bytes.  For
 *   example, a preamble byte near the end of corrupt data can be the beginning
 *   of the next packet on a continuous UART stream.
 *
 * Input:
 *   parser  parser containing the rejected candidate bytes.
 *   out     non-NULL destination passed through to protocol_parser_feed().
 *
 * Output:
 *   Resets and replays all bytes except the already-rejected first byte.  It
 *   returns true only if replay completes another valid packet in *out;
 *   otherwise parser retains any partial preamble/frame found during replay.
 */
static bool protocol_parser_resync(protocol_parser_t *parser, uart_packet_t *out) {
    uint8_t snapshot[PACKET_SIZE];
    /* Preserve bytes because resetting parser would make its buffer reusable. */

    uint8_t n = parser->len;
    /* Record how many candidate bytes were accumulated before reset. */

    if (n <= 1) {
        /* No later byte exists that could begin an overlapping replacement frame. */
        protocol_parser_reset(parser);
        return false;
    }

    memcpy(snapshot, parser->buf, n);
    /* Copy exactly the accumulated candidate bytes to stable local storage. */

    protocol_parser_reset(parser);
    /* Start fresh, then let normal parser rules inspect the possible overlap. */

    for (uint8_t i = 1; i < n; i++) {
        /* Skip byte 0: it was the rejected frame's assumed first preamble byte. */
        if (protocol_parser_feed(parser, snapshot[i], out)) {
            /* Replayed bytes happened to complete an overlapping valid packet. */
            return true;
        }
    }

    /* No complete packet yet; parser may still hold a useful partial prefix. */
    return false;
}

/*
 * Consume one UART byte and advance the fixed-frame protocol parser.
 *
 * Purpose:
 *   Turn a continuous, potentially noisy byte stream into validated
 *   uart_packet_t values while retaining partial preambles and frames between
 *   calls.
 *
 * Input:
 *   parser  non-NULL persistent parser state, retained across received bytes.
 *   byte    one newly received UART byte.
 *   out     non-NULL destination to fill when a complete valid packet appears.
 *
 * Output:
 *   Returns true only when this byte, or resynchronization triggered by it,
 *   completes a valid packet in *out.  Returns false while waiting for more
 *   data or after discarding invalid data; parser keeps any useful prefix.
 */
bool protocol_parser_feed(protocol_parser_t *parser, uint8_t byte, uart_packet_t *out) {
    if (parser->len == 0) {
        /* Idle state: ignore noise until the first preamble byte appears. */
        if (byte == PACKET_PREAMBLE_0) {
            parser->buf[0] = byte;
            /* Start a candidate frame with the recognized first preamble byte. */

            parser->len = 1;
            /* Next call must decide whether this is followed by preamble byte 2. */
        }

        return false;
    }

    if (parser->len == 1) {
        /* A first preamble byte was saved; validate the second one. */
        if (byte == PACKET_PREAMBLE_1) {
            parser->buf[1] = byte;
            /* The two-byte frame prefix is now complete. */

            parser->len = 2;
            /* Subsequent bytes are type, payload, and checksum. */
        } else if (byte == PACKET_PREAMBLE_0) {
            /* Preserve an overlapping first preamble, e.g. P0 P0 P1. */
            parser->buf[0] = byte;
            parser->len = 1;
        } else {
            /* No valid two-byte prefix remains, so discard this candidate. */
            protocol_parser_reset(parser);
        }

        return false;
    }

    parser->buf[parser->len++] = byte;
    /* Append type/payload/checksum bytes after the verified two-byte prefix. */

    if (parser->len < PACKET_SIZE) {
        /* A complete fixed-size frame has not arrived yet. */
        return false;
    }

    if (protocol_decode(parser->buf, out)) {
        /* Valid frame: consume it completely before reporting success. */
        protocol_parser_reset(parser);
        return true;
    }

    /* Invalid full frame: replay its overlap to recover the next possible frame. */
    return protocol_parser_resync(parser, out);
}

/*
 * Serialize one absolute mouse report into the protocol's eight-byte payload.
 *
 * Purpose:
 *   Give local USB mouse routing and UART mouse routing an identical absolute
 *   X/Y representation: buttons, little-endian coordinates, wheel, and two
 *   reserved zero bytes.
 *
 * Input:
 *   report   non-NULL absolute mouse state to send.
 *   payload  non-NULL PACKET_PAYLOAD_LEN-byte destination.
 *
 * Output layout:
 *   [0] buttons, [1..2] X little-endian, [3..4] Y little-endian,
 *   [5] signed wheel bit pattern, [6..7] reserved zero.
 *   Returns false for NULL input or coordinates outside POINTER_MAX; otherwise
 *   returns true after filling payload.
 */
bool protocol_pack_mouse(const mouse_abs_report_t *report, uint8_t payload[PACKET_PAYLOAD_LEN]) {
    if (report == NULL || payload == NULL) {
        /* Both a source report and a destination payload are required. */
        return false;
    }

    if (report->x > (uint16_t)POINTER_MAX || report->y > (uint16_t)POINTER_MAX) {
        /* Do not serialize coordinates the peer/USB absolute range cannot use. */
        return false;
    }

    memset(payload, 0, PACKET_PAYLOAD_LEN);
    /* Initialize both reserved bytes to their required zero value. */

    payload[0] = report->buttons;
    /* Copy the HID button-bit mask unchanged. */

    payload[1] = (uint8_t)(report->x & 0xFF);
    /* X byte 0: least-significant byte in little-endian order. */

    payload[2] = (uint8_t)((report->x >> 8) & 0xFF);
    /* X byte 1: most-significant byte. */

    payload[3] = (uint8_t)(report->y & 0xFF);
    /* Y byte 0: least-significant byte in little-endian order. */

    payload[4] = (uint8_t)((report->y >> 8) & 0xFF);
    /* Y byte 1: most-significant byte. */

    payload[5] = (uint8_t)report->wheel;
    /* Preserve the signed wheel delta's two's-complement bit pattern. */

    payload[6] = 0;
    payload[7] = 0;
    /* State the reserved-byte invariant explicitly, even though memset did it. */

    return true;
}

/*
 * Validate and deserialize an absolute mouse payload received over UART.
 *
 * Purpose:
 *   Reject incompatible or malformed mouse payloads before they can be sent to
 *   the local USB HID device, then restore the canonical mouse_abs_report_t.
 *
 * Input:
 *   payload  non-NULL eight-byte MSG_MOUSE_REPORT body.
 *   report   non-NULL destination for the decoded absolute report.
 *
 * Output:
 *   Returns true and fills *report when reserved bytes and X/Y bounds are
 *   valid.  Returns false without intentionally modifying *report otherwise.
 */
bool protocol_unpack_mouse(const uint8_t payload[PACKET_PAYLOAD_LEN], mouse_abs_report_t *report) {
    if (payload == NULL || report == NULL) {
        /* A payload source and a report destination are both required. */
        return false;
    }

    if (payload[6] != 0 || payload[7] != 0) {
        /* Non-zero reserved bytes identify an unsupported payload format. */
        return false;
    }

    uint16_t x = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    /* Rebuild X from its little-endian low and high bytes. */

    uint16_t y = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
    /* Rebuild Y from its little-endian low and high bytes. */

    if (x > (uint16_t)POINTER_MAX || y > (uint16_t)POINTER_MAX) {
        /* Reject positions outside the range accepted by the absolute HID report. */
        return false;
    }

    memset(report, 0, sizeof(*report));
    /* Clear report padding/reserved fields before populating visible fields. */

    report->buttons = payload[0];
    /* Restore the original button-bit mask. */

    report->x = x;
    report->y = y;
    /* Store the validated absolute coordinates. */

    report->wheel = (int8_t)payload[5];
    /* Interpret byte 5 again as a signed wheel delta. */

    return true;
}

/*
 * Serialize the local board's link state into the fixed eight-byte heartbeat
 * payload.  This function only writes the payload; uart_queue_packet() later
 * adds framing, type, and checksum before transmission.
 *
 * Input:
 *   payload        caller-provided buffer with PACKET_PAYLOAD_LEN bytes.
 *   role           local board role: ROLE_A or ROLE_B.
 *   active_output  PC currently selected to receive keyboard and mouse.
 *   generation     monotonically increasing version of the output decision.
 *
 * Output layout (protocol v3):
 *   [0] protocol version, [1] role, [2] active output,
 *   [3..6] generation (little-endian), [7] reserved zero.
 */
void protocol_pack_heartbeat(uint8_t payload[PACKET_PAYLOAD_LEN],
                             uint8_t role,
                             uint8_t active_output,
                             uint32_t generation) {
    memset(payload, 0, PACKET_PAYLOAD_LEN);

    payload[0] = (uint8_t)DESKHOP_PROTOCOL_VERSION;
    payload[1] = role;
    payload[2] = active_output;
    payload[3] = (uint8_t)(generation & 0xFF);
    payload[4] = (uint8_t)((generation >> 8) & 0xFF);
    payload[5] = (uint8_t)((generation >> 16) & 0xFF);
    payload[6] = (uint8_t)((generation >> 24) & 0xFF);
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

    *version = payload[0];
    *role = payload[1];
    *active_output = payload[2];
    *generation = (uint32_t)payload[3] |
                  ((uint32_t)payload[4] << 8) |
                  ((uint32_t)payload[5] << 16) |
                  ((uint32_t)payload[6] << 24);

    if (*active_output > OUTPUT_B) {
        return false;
    }

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

    /* Heartbeat v3: version, role, output, gen LE, reserved */
    uint8_t hb[PACKET_PAYLOAD_LEN];
    protocol_pack_heartbeat(hb, ROLE_A, OUTPUT_B, 0x01020304u);
    if (hb[0] != DESKHOP_PROTOCOL_VERSION ||
        hb[1] != ROLE_A || hb[2] != OUTPUT_B ||
        hb[3] != 0x04 || hb[4] != 0x03 || hb[5] != 0x02 || hb[6] != 0x01 ||
        hb[7] != 0) {
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

    uint8_t bad_out[PACKET_PAYLOAD_LEN];
    memcpy(bad_out, hb, PACKET_PAYLOAD_LEN);
    bad_out[2] = 2;
    if (protocol_unpack_heartbeat(bad_out, &role, &output, &gen, &ver)) {
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
