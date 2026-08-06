/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

/* Zigbee configuration constants - Color Dimmable Light Device */
/* Use default channel mask for channel 11 */
#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   (1U << 11)
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0xffffffff ^ (1U << 11))

#define ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID (10)

#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define ESP_BATTERY_ATTR_UPDATE_INTERVAL (5) /* Battery attributes update interval (seconds) */

#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ESP_MODEL_IDENTIFIER "\x07"CONFIG_IDF_TARGET

/* ZED (End Device) configuration */
#define ESP_ZIGBEE_ZED_CONFIG()                              \
    {                                                        \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,       \
        .install_code_policy = false,                        \
        .zed_config = {                                      \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,          \
            .keep_alive = 4000,                              \
        },                                                  \
    }

/* Platform configuration */
#if CONFIG_SOC_IEEE802154_SUPPORTED
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                 \
    {                                                                \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                                            \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,              \
        },                                                           \
    }
#else
#warning "The example is not for IEEE 802.15.4-disabled SoC usage"
#endif

/* Default configuration for ZED */
#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    };
