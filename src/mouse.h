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

void mouse_init(void);
void mouse_on_report(const uint8_t *raw, uint16_t len);
void mouse_on_unmount(void);
void mouse_task(device_state_t *state);
void mouse_queue_local(const mouse_rel_report_t *report);
void mouse_release_local(void);
