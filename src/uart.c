#include "uart.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "board.h"
#include "config.h"
#include "router.h"

#define SERIAL_UART uart0
#define SERIAL_DATA_BITS 8
#define SERIAL_STOP_BITS 1
#define SERIAL_PARITY UART_PARITY_NONE

typedef struct {
    uart_packet_t items[UART_TX_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} packet_queue_t;

static packet_queue_t s_tx_queue;
static protocol_parser_t s_parser;
static uint32_t s_last_heartbeat_tx_ms;

static bool queue_push(packet_queue_t *q, const uart_packet_t *pkt) {
    if (q->count >= UART_TX_QUEUE_LEN) {
        return false;
    }
    q->items[q->tail] = *pkt;
    q->tail = (uint8_t)((q->tail + 1) % UART_TX_QUEUE_LEN);
    q->count++;
    return true;
}

static bool queue_pop(packet_queue_t *q, uart_packet_t *pkt) {
    if (q->count == 0) {
        return false;
    }
    *pkt = q->items[q->head];
    q->head = (uint8_t)((q->head + 1) % UART_TX_QUEUE_LEN);
    q->count--;
    return true;
}

/*
 * Select the physical UART TX/RX GPIO pair wired for one board role.
 *
 * Purpose:
 *   Board A and board B use role-specific pin definitions, even though both
 *   execute the same firmware.  uart_link_init() needs this mapping before it
 *   assigns GPIO_FUNC_UART to the correct pins.
 *
 * Input:
 *   board_role  ROLE_A selects BOARD_A_TX/RX; every other value selects the
 *               board-B pair, matching the firmware's two-role assumption.
 *   tx, rx      non-NULL destinations for the selected GPIO numbers.
 *
 * Output:
 *   Writes the chosen TX and RX pin numbers through *tx and *rx.  It performs
 *   no GPIO or UART hardware operation itself.
 */
static void uart_pins_for_role(uint8_t board_role, uint *tx, uint *rx) {
    if (board_role == ROLE_A) {
        *tx = BOARD_A_TX;
        /* Board A transmits on its configured cross-link TX GPIO. */

        *rx = BOARD_A_RX;
        /* Board A receives on its configured cross-link RX GPIO. */
    } else {
        *tx = BOARD_B_TX;
        /* Board B uses its own TX GPIO in the same shared firmware image. */

        *rx = BOARD_B_RX;
        /* Board B uses its own RX GPIO. */
    }
}

void uart_link_init(uint8_t board_role) {
    uint tx_pin;
    uint rx_pin;
    uart_pins_for_role(board_role, &tx_pin, &rx_pin);

    uart_init(SERIAL_UART, SERIAL_BAUDRATE);
    uart_set_hw_flow(SERIAL_UART, false, false);
    uart_set_format(SERIAL_UART, SERIAL_DATA_BITS, SERIAL_STOP_BITS, SERIAL_PARITY);
    uart_set_fifo_enabled(SERIAL_UART, true);

    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);

    memset(&s_tx_queue, 0, sizeof(s_tx_queue));
    protocol_parser_reset(&s_parser);
    s_last_heartbeat_tx_ms = 0;
}

/*
 * Write raw framed bytes to the configured UART peripheral.
 *
 * Purpose:
 *   Provide the final hardware-send step used by flush_tx() after a queued
 *   packet has been encoded with preamble, type, payload, and checksum.
 *
 * Input:
 *   data  non-NULL byte buffer to transmit.
 *   len   number of bytes in data; zero means there is nothing to send.
 *
 * Output:
 *   No return value.  For valid input, uart_write_blocking() writes all len
 *   bytes to SERIAL_UART.  NULL data or zero length is a harmless no-op.
 */
void uart_write_bytes(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        /* Avoid handing an invalid or empty buffer to the blocking SDK call. */
        return;
    }

    uart_write_blocking(SERIAL_UART, data, len);
    /* Wait until every requested raw frame byte has been accepted for transmit. */
}

bool uart_queue_packet(uint8_t type, const uint8_t payload[PACKET_PAYLOAD_LEN]) {
    if (!protocol_type_valid(type) || payload == NULL) {
        return false;
    }

    uart_packet_t pkt;
    pkt.type = type;
    memcpy(pkt.payload, payload, PACKET_PAYLOAD_LEN);
    return queue_push(&s_tx_queue, &pkt);
}

bool uart_queue_heartbeat(const device_state_t *state) {
    uint8_t payload[PACKET_PAYLOAD_LEN] = {0};
    protocol_pack_heartbeat(payload,
                            state->board_role,
                            state->active_output,
                            state->output_generation);
    return uart_queue_packet(MSG_HEARTBEAT, payload);
}

bool uart_queue_keyboard(const hid_keyboard_report_t *report) {
    if (report == NULL) {
        return false;
    }
    uint8_t payload[PACKET_PAYLOAD_LEN] = {0};
    memcpy(payload, report, sizeof(hid_keyboard_report_t));
    return uart_queue_packet(MSG_KEYBOARD_REPORT, payload);
}

bool uart_queue_mouse(const mouse_abs_report_t *report) {
    if (report == NULL) {
        return false;
    }
    if (!g_state.peer_protocol_ok) {
        return false;
    }

    uint8_t payload[PACKET_PAYLOAD_LEN];
    if (!protocol_pack_mouse(report, payload)) {
        return false;
    }
    return uart_queue_packet(MSG_MOUSE_REPORT, payload);
}

/*
 * Apply one fully parsed UART packet and refresh peer liveness.
 *
 * Purpose:
 *   This is the bridge between byte-level protocol parsing and routing.  Every
 *   valid packet proves the peer link is alive; its type then selects the
 *   keyboard, mouse, output-selection, or heartbeat handler.
 *
 * Input:
 *   state  non-NULL local shared state to update.
 *   pkt    non-NULL packet already validated by protocol_parser_feed().
 *
 * Output:
 *   Marks the peer online, records the liveness timestamp, and dispatches the
 *   packet.  A first non-heartbeat packet after offline also causes this board
 *   to broadcast its current output selection for fast resynchronization.
 */
static void handle_rx_packet(device_state_t *state, const uart_packet_t *pkt) {
    bool was_online = state->peer_online;
    /* Preserve previous liveness so this packet can detect an offline -> online transition. */

    state->last_peer_heartbeat_ms = board_millis();
    /* Any valid packet, not only a heartbeat, confirms the UART peer is alive. */

    state->peer_online = true;
    /* Keep later route/protocol decisions aware that the peer is reachable. */

    bool just_online = !was_online;
    /* True only for the first valid packet received after a timeout/offline state. */

    switch (pkt->type) {
    case MSG_SELECT_OUTPUT:
        /* Apply the peer's generation-tagged output-selection decision. */
        router_on_select_output(state, pkt->payload);
        break;

    case MSG_KEYBOARD_REPORT:
        /* Board B may forward keyboard state received from board A to local USB. */
        router_on_remote_keyboard(state, pkt->payload);
        break;

    case MSG_MOUSE_REPORT:
        if (state->peer_protocol_ok) {
            /* Only accept absolute mouse payloads after heartbeat version agreement. */
            router_on_remote_mouse(state, pkt->payload);
        }
        break;

    case MSG_HEARTBEAT:
        /* Negotiate protocol/link state and reconcile output state if required. */
        router_on_peer_heartbeat(state, pkt->payload, just_online);
        break;

    default:
        /* protocol_decode() rejects unknown types; retain this defensive fallback. */
        break;
    }

    if (just_online && pkt->type != MSG_HEARTBEAT) {
        /* The peer sent data before heartbeat; proactively give it our output state. */
        router_broadcast_active_output(state);
    }
}

static void drain_rx(device_state_t *state) {
    while (uart_is_readable(SERIAL_UART)) {
        uint8_t byte = (uint8_t)uart_getc(SERIAL_UART);
        uart_packet_t pkt;
        if (protocol_parser_feed(&s_parser, byte, &pkt)) {
            handle_rx_packet(state, &pkt);
        }
    }
}

static void flush_tx(void) {
    uart_packet_t pkt;
    while (queue_pop(&s_tx_queue, &pkt)) {
        uint8_t raw[PACKET_SIZE];
        protocol_encode(raw, pkt.type, pkt.payload);
        uart_write_bytes(raw, PACKET_SIZE);
    }
}

/*
 * Queue a link-state heartbeat when the periodic transmit interval expires.
 *
 * Purpose:
 *   Keep the peer's liveness timer refreshed and advertise this board's role,
 *   selected output, generation, and protocol version even while no HID input
 *   is being routed.
 *
 * Input:
 *   state  non-NULL local state to serialize into the heartbeat payload.
 *
 * Output:
 *   Queues one MSG_HEARTBEAT at most once per HEARTBEAT_INTERVAL_MS.  The last
 *   transmit timestamp advances only after queueing succeeds, so a full queue
 *   is retried by a later uart_link_task() iteration.
 */
static void maybe_send_heartbeat(device_state_t *state) {
    uint32_t now = board_millis();
    /* Sample the monotonic millisecond clock once for this interval decision. */

    if ((now - s_last_heartbeat_tx_ms) < HEARTBEAT_INTERVAL_MS) {
        /* Unsigned subtraction is rollover-safe; it is not time to send yet. */
        return;
    }

    if (uart_queue_heartbeat(state)) {
        /* Record only successfully queued heartbeats so a failed queue is retried. */
        s_last_heartbeat_tx_ms = now;
    }
}

/*
 * Mark the peer offline after it has been silent longer than the link timeout.
 *
 * Purpose:
 *   Remove stale protocol/link assumptions and release locally routed input so
 *   a disconnected peer cannot leave a key, button, or output state stuck.
 *
 * Input:
 *   state  non-NULL local state whose liveness timestamp is refreshed by every
 *          valid received UART packet.
 *
 * Output:
 *   When the peer exceeds PEER_TIMEOUT_MS without a valid packet, clears peer
 *   liveness/protocol fields, resets the incremental parser, and invokes the
 *   router's offline safety handling.  Otherwise it leaves state unchanged.
 */
static void update_peer_timeout(device_state_t *state) {
    if (!state->peer_online) {
        /* An already-offline peer has no timeout transition to process. */
        return;
    }

    uint32_t now = board_millis();
    /* Use the same monotonic clock used when handle_rx_packet() recorded liveness. */

    if ((now - state->last_peer_heartbeat_ms) <= PEER_TIMEOUT_MS) {
        /* Recent valid traffic means the peer is still considered online. */
        return;
    }

    state->peer_online = false;
    /* Publish the offline transition before router safety policy runs. */

    state->peer_protocol_ok = false;
    /* Mouse forwarding is no longer safe until a compatible heartbeat returns. */

    state->protocol_mismatch = false;
    /* Clear the old diagnosis; a future heartbeat will establish a new one. */

    state->peer_protocol_version = 0;
    /* The previous peer version is meaningless while no peer is online. */

    protocol_parser_reset(&s_parser);
    /* Discard an incomplete frame that may have been left by the dead link. */

    router_on_peer_offline(state);
    /* Release the appropriate local input state for a safe disconnected state. */
}

/*
 * Run one cooperative-service iteration for the board-to-board UART link.
 *
 * Purpose:
 *   Keep receiving, routing, heartbeat generation, transmission, and peer
 *   liveness checking in one deterministic order.  main() calls this task
 *   repeatedly rather than using a dedicated UART thread.
 *
 * Input:
 *   state  non-NULL shared board state used by packet handlers, heartbeat
 *          serialization, and peer-timeout safety handling.
 *
 * Output:
 *   Drains all currently received UART bytes, queues a due heartbeat, transmits
 *   pending packets, and marks a silent peer offline when necessary.  It has no
 *   return value; its effects are state updates and UART queue/hardware I/O.
 */
void uart_link_task(device_state_t *state) {
    drain_rx(state);
    /* Process inbound packets first so their liveness/state is current this iteration. */

    maybe_send_heartbeat(state);
    /* Add periodic link-state traffic to the TX queue only when its interval expires. */

    flush_tx();
    /* Encode and write every queued outgoing packet, including responses/heartbeat. */

    update_peer_timeout(state);
    /* After accepting all current RX traffic, apply the peer-offline safety policy. */
}
