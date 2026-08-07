#include "pico/stdlib.h"

#include "board.h"
#include "keyboard.h"
#include "mouse.h"
#include "protocol.h"
#include "state.h"
#include "uart.h"
#include "usb_device.h"
#include "usb_host.h"

device_state_t g_state;

int main(void) {
    board_init(&g_state);

#ifdef KVM_DEBUG
    if (!board_pinmap_selftest() ||
        !board_ownership_selftest() ||
        !board_probe_selftest() ||
        !board_detect_selftest() ||
        !mouse_pointer_selftest() ||
        !mouse_edge_selftest() ||
        !protocol_selftest()) {
        while (true) {
            tight_loop_contents();
        }
    }
#endif

    board_boot_resolve_role(&g_state);

    const board_pinmap_t *pins = board_get_pinmap(g_state.board_role);
    if (pins == NULL) {
        while (true) {
            tight_loop_contents();
        }
    }

    keyboard_init();
    mouse_init();

    usb_device_init();
    usb_host_init();
    uart_link_init(pins);

    g_state.routing_enabled = true;

    board_enable_watchdog();

    while (true) {
        usb_device_task(&g_state);
        usb_host_task();
        keyboard_task(&g_state);
        mouse_task(&g_state);
        uart_link_task(&g_state);
        board_update_led(&g_state);
        board_kick_watchdog();
    }
}
