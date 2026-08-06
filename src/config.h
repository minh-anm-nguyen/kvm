#pragma once

#include <stdint.h>

#include "class/hid/hid.h"

/* ---- Board / output roles (compile-time for board_role) ---------------- */
#ifndef BOARD_ROLE
#error "BOARD_ROLE must be set by the build system (0=A, 1=B)"
#endif

#define ROLE_A 0
#define ROLE_B 1

#define OUTPUT_A ROLE_A
#define OUTPUT_B ROLE_B

/* Default active machine after boot */
#define DEFAULT_ACTIVE_OUTPUT OUTPUT_A

/* ---- Hardware pins (Raspberry Pi Pico) --------------------------------- */
#define GPIO_LED_PIN 25

/* UART between boards — same pinout as DeskHop hardware */
#define BOARD_A_TX 12
#define BOARD_A_RX 13
#define BOARD_B_TX 16
#define BOARD_B_RX 17

/* PIO-USB host D+/D- (D- is next pin) */
#define PIO_USB_DP_PIN 14

#define SERIAL_UART_ID 0 /* uart0 */
#define SERIAL_BAUDRATE 115200

/* ---- Fixed hotkey: Left Ctrl + Caps Lock -------------------------------- */
#define HOTKEY_MODIFIER KEYBOARD_MODIFIER_LEFTCTRL
#define HOTKEY_KEYCODE  HID_KEY_CAPS_LOCK

/* ---- Queue lengths (fixed, no allocation) ------------------------------ */
#define KEYBOARD_TX_QUEUE_LEN 16
#define MOUSE_TX_QUEUE_LEN    32
#define UART_TX_QUEUE_LEN     16

/* ---- Timing ------------------------------------------------------------ */
/* LED identification patterns for Step 1 (distinct A vs B). */
#define LED_BLINK_MS_ROLE_A 500  /* slow blink  */
#define LED_BLINK_MS_ROLE_B 100  /* fast blink  */

/* Heartbeat / peer recovery */
#define HEARTBEAT_INTERVAL_MS 500
#define PEER_TIMEOUT_MS       2000

/* Hardware watchdog */
#define WATCHDOG_TIMEOUT_MS 3000

/* Protocol */
#define PACKET_PREAMBLE_0 0xAA
#define PACKET_PREAMBLE_1 0x55
#define PACKET_SIZE       12
#define DESKHOP_PROTOCOL_VERSION 2

/* LED mismatch blink (protocol v1/v2) */
#define LED_PROTOCOL_MISMATCH_MS 100

/* USB device */
#define USB_HID_POLL_MS 10

/* Absolute pointer logical range (HID abs mouse + edge switch) */
#define POINTER_MIN       0
#define POINTER_MAX       32767
#define POINTER_CENTER    16384
#define POINTER_ENTRY_GAP 32
#define POINTER_SCALE_X   24
#define POINTER_SCALE_Y   24

#define ENABLE_EDGE_SWITCHING 1
