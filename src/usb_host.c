#include "usb_host.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"

#include "board.h"
#include "config.h"
#include "keyboard.h"
#include "mouse.h"

void usb_host_init(void) {
    set_sys_clock_khz(120000, true);

    static pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
    config.pin_dp = PIO_USB_DP_PIN;

    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &config);
}

void usb_host_task(void) {
    tuh_task();
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        if (board_accepts_keyboard(g_state.board_role)) {
            tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
            g_state.kbd_dev_addr = dev_addr;
            g_state.kbd_instance = instance;
            g_state.input_connected = true;
            tuh_hid_receive_report(dev_addr, instance);
        } else if (board_role_is_concrete(g_state.board_role)) {
            board_note_wrong_port_input();
        }
        return;
    }

    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        if (board_accepts_mouse(g_state.board_role)) {
            tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
            g_state.mouse_dev_addr = dev_addr;
            g_state.mouse_instance = instance;
            g_state.mouse_connected = true;
            tuh_hid_receive_report(dev_addr, instance);
        } else if (board_role_is_concrete(g_state.board_role)) {
            board_note_wrong_port_input();
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (board_accepts_keyboard(g_state.board_role) &&
        dev_addr == g_state.kbd_dev_addr &&
        instance == g_state.kbd_instance) {
        keyboard_on_unmount();
        g_state.kbd_dev_addr = 0;
        g_state.kbd_instance = 0;
        g_state.input_connected = false;
        return;
    }

    if (board_accepts_mouse(g_state.board_role) &&
        dev_addr == g_state.mouse_dev_addr &&
        instance == g_state.mouse_instance) {
        mouse_on_unmount();
        g_state.mouse_dev_addr = 0;
        g_state.mouse_instance = 0;
        g_state.mouse_connected = false;
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    if (board_accepts_keyboard(g_state.board_role) &&
        dev_addr == g_state.kbd_dev_addr &&
        instance == g_state.kbd_instance) {
        if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
            keyboard_on_report(report, len);
        }
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    if (board_accepts_mouse(g_state.board_role) &&
        dev_addr == g_state.mouse_dev_addr &&
        instance == g_state.mouse_instance) {
        if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE) {
            mouse_on_report(report, len);
        }
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol) {
    (void)dev_addr;
    (void)idx;
    (void)protocol;
}
