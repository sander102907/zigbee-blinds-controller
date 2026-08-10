#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t light_driver_init(void);
void light_driver_set_state(uint8_t state);
void light_driver_set_brightness(uint8_t level);
void light_driver_set_color(uint16_t color_x, uint16_t color_y);
void light_driver_set_zigbee_connecting(bool connecting);
