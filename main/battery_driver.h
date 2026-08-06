
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Battery driver update callback
 *
 * @param[in] battery_percentage Battery percentage value from sensor
 *
 */
typedef void (*esp_battery_callback_t)(uint8_t battery_percentage);

/**
 * @brief Initialize battery driver and set callback function
 *
 * @param update_interval Sensor value update interval.
 * @param cb              Callback function pointer.
 *
 * @return ESP_OK if the driver initialization succeed, otherwise ESP_FAIL.
 */
esp_err_t battery_driver_init(uint16_t update_interval, esp_battery_callback_t cb);

#ifdef __cplusplus
} // extern "C"
#endif