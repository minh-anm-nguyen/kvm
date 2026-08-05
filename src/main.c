#include "pico/stdlib.h"

#include "board.h"
#include "keyboard.h"
#include "mouse.h"
#include "state.h"
#include "uart.h"
#include "usb_device.h"
#include "usb_host.h"

device_state_t g_state;

int main(void) {
    board_init(&g_state);
    keyboard_init();
    mouse_init();

    usb_host_init();
    usb_device_init();
    uart_link_init(g_state.board_role);

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
