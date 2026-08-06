#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

typedef struct {
    uint8_t buttons;
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} mouse_rel_report_t;

typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    uint16_t x;
    uint16_t y;
    int8_t   wheel;
    uint8_t  reserved[2];
} mouse_abs_report_t;

_Static_assert(sizeof(mouse_abs_report_t) == 8, "mouse_abs_report_t must be 8 bytes");

int32_t mouse_scale_delta(int8_t delta, int32_t scale);
int32_t mouse_clamp(int32_t value, int32_t lo, int32_t hi);
void mouse_pointer_advance(int32_t *pointer_x,
                           int32_t *pointer_y,
                           int8_t dx,
                           int8_t dy);

void mouse_init(void);
void mouse_on_report(const uint8_t *raw, uint16_t len);
void mouse_on_unmount(void);
void mouse_task(device_state_t *state);
void mouse_queue_local(const mouse_abs_report_t *report);
void mouse_release_local(void);
void mouse_build_report(const device_state_t *state,
                        uint8_t buttons,
                        int8_t wheel,
                        mouse_abs_report_t *out);
bool mouse_pointer_selftest(void);
