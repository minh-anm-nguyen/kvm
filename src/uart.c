#include "uart.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "board.h"
#include "config.h"
#include "keyboard.h"
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

static void uart_pins_for_role(uint8_t board_role, uint *tx, uint *rx) {
    if (board_role == ROLE_A) {
        *tx = BOARD_A_TX;
        *rx = BOARD_A_RX;
    } else {
        *tx = BOARD_B_TX;
        *rx = BOARD_B_RX;
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

void uart_write_bytes(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return;
    }
    uart_write_blocking(SERIAL_UART, data, len);
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
    payload[0] = state->board_role;
    payload[1] = state->active_output;
    payload[2] = (uint8_t)(state->output_generation & 0xFF);
    payload[3] = (uint8_t)((state->output_generation >> 8) & 0xFF);
    payload[4] = (uint8_t)((state->output_generation >> 16) & 0xFF);
    payload[5] = (uint8_t)((state->output_generation >> 24) & 0xFF);
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

static void handle_rx_packet(device_state_t *state, const uart_packet_t *pkt) {
    state->last_peer_heartbeat_ms = board_millis();
    state->peer_online = true;

    switch (pkt->type) {
    case MSG_SELECT_OUTPUT:
        router_on_select_output(state, pkt->payload);
        break;
    case MSG_KEYBOARD_REPORT:
        router_on_remote_keyboard(state, pkt->payload);
        break;
    case MSG_HEARTBEAT: {
        if (pkt->payload[1] > OUTPUT_B) {
            break;
        }
        uint32_t gen = (uint32_t)pkt->payload[2] |
                       ((uint32_t)pkt->payload[3] << 8) |
                       ((uint32_t)pkt->payload[4] << 16) |
                       ((uint32_t)pkt->payload[5] << 24);
        if (gen > state->output_generation) {
            uint8_t sel[PACKET_PAYLOAD_LEN] = {0};
            sel[0] = pkt->payload[1];
            sel[1] = pkt->payload[2];
            sel[2] = pkt->payload[3];
            sel[3] = pkt->payload[4];
            sel[4] = pkt->payload[5];
            router_on_select_output(state, sel);
        }
        break;
    }
    default:
        break;
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

static void maybe_send_heartbeat(device_state_t *state) {
    uint32_t now = board_millis();
    if ((now - s_last_heartbeat_tx_ms) < HEARTBEAT_INTERVAL_MS) {
        return;
    }
    if (uart_queue_heartbeat(state)) {
        s_last_heartbeat_tx_ms = now;
    }
}

static void update_peer_timeout(device_state_t *state) {
    if (!state->peer_online) {
        return;
    }
    uint32_t now = board_millis();
    if ((now - state->last_peer_heartbeat_ms) > PEER_TIMEOUT_MS) {
        state->peer_online = false;
        protocol_parser_reset(&s_parser);
        hid_keyboard_report_t empty = {0};
        state->remote_keyboard = empty;
        if (state->board_role == ROLE_B) {
            keyboard_queue_local(&empty);
        }
    }
}

void uart_link_task(device_state_t *state) {
    drain_rx(state);
    maybe_send_heartbeat(state);
    flush_tx();
    update_peer_timeout(state);
}
