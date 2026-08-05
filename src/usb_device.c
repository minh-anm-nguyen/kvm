#include "usb_device.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#include "config.h"

#define USB_VID 0x1209
#define USB_PID 0xC001

#define EPNUM_KEYBOARD 0x81
#define EPNUM_MOUSE    0x82

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_KEYBOARD,
    STRID_MOUSE,
};

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static uint8_t const desc_hid_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

static uint8_t const desc_hid_mouse[] = {
    TUD_HID_REPORT_DESC_MOUSE(),
};

enum {
    CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN,
};

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_HID_DESCRIPTOR(ITF_NUM_KEYBOARD,
                       STRID_KEYBOARD,
                       HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_keyboard),
                       EPNUM_KEYBOARD,
                       CFG_TUD_HID_EP_BUFSIZE,
                       10),

    TUD_HID_DESCRIPTOR(ITF_NUM_MOUSE,
                       STRID_MOUSE,
                       HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_mouse),
                       EPNUM_MOUSE,
                       CFG_TUD_HID_EP_BUFSIZE,
                       10),
};

#if BOARD_ROLE == ROLE_A
static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "DeskHop Minimal",
    "DeskHop Minimal A",
    NULL,
    "Keyboard",
    "Mouse",
};
#else
static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "DeskHop Minimal",
    "DeskHop Minimal B",
    NULL,
    "Keyboard",
    "Mouse",
};
#endif

static uint16_t _desc_str[32];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == ITF_NUM_KEYBOARD) {
        return desc_hid_keyboard;
    }
    if (instance == ITF_NUM_MOUSE) {
        return desc_hid_mouse;
    }
    return desc_hid_keyboard;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count = 0;
    static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        if (serial[0] == '\0') {
            pico_get_unique_board_id_string(serial, sizeof(serial));
        }
        chr_count = (uint8_t)strlen(serial);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)serial[i];
        }
    } else {
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        if (str == NULL) {
            return NULL;
        }
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void tud_mount_cb(void) {
    g_state.usb_device_ready = true;
}

void tud_umount_cb(void) {
    g_state.usb_device_ready = false;
}

void usb_device_init(void) {
    tusb_init();
}

bool usb_device_ready(void) {
    return tud_mounted() && tud_hid_n_ready(ITF_NUM_KEYBOARD) && tud_hid_n_ready(ITF_NUM_MOUSE);
}

bool usb_device_send_keyboard(const hid_keyboard_report_t *report) {
    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_KEYBOARD) || report == NULL) {
        return false;
    }
    uint8_t keycode[6];
    memcpy(keycode, report->keycode, sizeof(keycode));
    return tud_hid_n_keyboard_report(ITF_NUM_KEYBOARD, 0, report->modifier, keycode);
}

bool usb_device_send_keyboard_empty(void) {
    uint8_t keycode[6] = {0};
    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_KEYBOARD)) {
        return false;
    }
    return tud_hid_n_keyboard_report(ITF_NUM_KEYBOARD, 0, 0, keycode);
}

bool usb_device_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
    if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_MOUSE)) {
        return false;
    }
    return tud_hid_n_mouse_report(ITF_NUM_MOUSE, 0, buttons, x, y, wheel, 0);
}

#ifdef KVM_DEBUG
static void usb_device_self_test(device_state_t *state) {
    enum {
        ST_IDLE = 0,
        ST_WAIT_HOST,
        ST_KEY_DOWN,
        ST_KEY_UP,
        ST_MOUSE_MOVE,
        ST_CLICK_DOWN,
        ST_CLICK_UP,
        ST_MOUSE_BACK,
        ST_DONE,
    };

    static uint8_t step;
    static uint32_t mark_ms;
    static bool started;

    if (step == ST_DONE) {
        return;
    }

    if (!state->usb_device_ready || !tud_mounted()) {
        step = ST_IDLE;
        started = false;
        return;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!started) {
        started = true;
        step = ST_WAIT_HOST;
        mark_ms = now;
    }

    switch (step) {
    case ST_WAIT_HOST:
        if ((now - mark_ms) < 2000) {
            return;
        }
        if (!usb_device_ready()) {
            return;
        }
        {
            hid_keyboard_report_t kr = {0};
            kr.keycode[0] = (state->board_role == ROLE_A) ? HID_KEY_A : HID_KEY_B;
            if (usb_device_send_keyboard(&kr)) {
                step = ST_KEY_DOWN;
                mark_ms = now;
            }
        }
        break;

    case ST_KEY_DOWN:
        if ((now - mark_ms) < 80) {
            return;
        }
        if (usb_device_send_keyboard_empty()) {
            step = ST_KEY_UP;
            mark_ms = now;
        }
        break;

    case ST_KEY_UP:
        if ((now - mark_ms) < 100) {
            return;
        }
        if (usb_device_send_mouse(0, 40, 0, 0)) {
            step = ST_MOUSE_MOVE;
            mark_ms = now;
        }
        break;

    case ST_MOUSE_MOVE:
        if ((now - mark_ms) < 80) {
            return;
        }
        if (usb_device_send_mouse(MOUSE_BUTTON_LEFT, 0, 0, 0)) {
            step = ST_CLICK_DOWN;
            mark_ms = now;
        }
        break;

    case ST_CLICK_DOWN:
        if ((now - mark_ms) < 60) {
            return;
        }
        if (usb_device_send_mouse(0, 0, 0, 0)) {
            step = ST_CLICK_UP;
            mark_ms = now;
        }
        break;

    case ST_CLICK_UP:
        if ((now - mark_ms) < 80) {
            return;
        }
        if (usb_device_send_mouse(0, -40, 0, 0)) {
            step = ST_MOUSE_BACK;
            mark_ms = now;
        }
        break;

    case ST_MOUSE_BACK:
        if ((now - mark_ms) < 80) {
            return;
        }
        step = ST_DONE;
        break;

    default:
        break;
    }
}
#endif

void usb_device_task(device_state_t *state) {
    tud_task();

#ifdef KVM_DEBUG
    usb_device_self_test(state);
#else
    (void)state;
#endif
}
