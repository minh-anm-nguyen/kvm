#include "pico/stdlib.h"

#include "board.h"
#include "state.h"

device_state_t g_state;

int main(void) {
    board_init(&g_state);

    while (true) {
        board_update_led(&g_state);
        board_kick_watchdog();

        tight_loop_contents();
    }
}
