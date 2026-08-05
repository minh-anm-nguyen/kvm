#include "pico/stdlib.h"

#include "board.h"
#include "state.h"
#include "uart.h"
#include "usb_device.h"

device_state_t g_state;

int main(void) {
    board_init(&g_state);
    usb_device_init();
    uart_link_init(g_state.board_role);

    while (true) {
        usb_device_task(&g_state);
        uart_link_task(&g_state);
        board_update_led(&g_state);
        board_kick_watchdog();
    }
}
