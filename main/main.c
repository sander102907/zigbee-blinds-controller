/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "battery_driver.h"
#include "alarm_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motor_driver.c"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
static esp_pm_lock_handle_t pm_lock;

#endif

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

#if CONFIG_ESP_SLEEP_DEBUG
#include "esp_private/esp_pmu.h"
#include "esp_private/esp_sleep_internal.h"
#endif

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/af.h"

#include "light_driver.h"
#include "main.h"

static const char *TAG = "ZIGBEE_BLINDS_CTRL";

#define BUTTON_FORWARD_GPIO CONFIG_BUTTON_UP_GPIO
#define BUTTON_BACKWARD_GPIO CONFIG_BUTTON_DOWN_GPIO
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_DEBOUNCE_MS 100
#define MOTOR_FULL_TRAVEL_MS 4000U
#define WINDOW_COVERING_TRAVEL_TIME_ATTR_ID 0xF010U
#define WINDOW_COVERING_ENDPOINT_CALIBRATION_ATTR_ID 0xF011U
#define WINDOW_COVERING_TRAVEL_TIME_MANUF_CODE EZB_ZCL_ESP_MANUF_CODE
#define CONFIG_NAMESPACE "blind_cfg"
#define CONFIG_KEY_TRAVEL_MS "travel_ms"
#define CONFIG_KEY_ENDPOINT_CALIBRATION_MS "endpoint_calibration_ms"
#define CONFIG_KEY_TILT_PERCENTAGE "tilt_percentage"

static uint8_t s_tilt_percentage = 0;
static uint8_t s_target_tilt_percentage = 0;
static TickType_t s_zigbee_move_deadline = 0;
static uint32_t s_full_travel_ms = MOTOR_FULL_TRAVEL_MS;
static uint32_t s_endpoint_calibration_ms = 0;

typedef enum
{
    MOTOR_DIRECTION_STOP = 0,
    MOTOR_DIRECTION_FORWARD,
    MOTOR_DIRECTION_BACKWARD,
} motor_direction_t;

typedef enum
{
    MOTOR_CONTROL_SOURCE_NONE = 0,
    MOTOR_CONTROL_SOURCE_BUTTONS,
    MOTOR_CONTROL_SOURCE_ZIGBEE,
} motor_control_source_t;

static motor_direction_t s_motor_direction = MOTOR_DIRECTION_STOP;
static motor_control_source_t s_motor_control_source = MOTOR_CONTROL_SOURCE_NONE;
static TaskHandle_t s_button_task_handle;
#ifdef CONFIG_PM_ENABLE
static bool s_motion_pm_lock_held;
#endif

/**
 * @brief Handle on/off cluster attribute changes
 */
static void zcl_on_off_attr_value_handler(const ezb_zcl_attribute_t *attribute)
{
    ESP_RETURN_ON_FALSE(attribute, , TAG, "attribute is invalid");
    switch (attribute->id)
    {
    case EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID:
        light_driver_set_state(*(uint8_t *)attribute->data.value);
        ESP_LOGI(TAG, "Set On/Off: %d", *(uint8_t *)attribute->data.value);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported on/off attribute ID(0x%04x)", attribute->id);
        break;
    }
}

/**
 * @brief Handle level/brightness cluster attribute changes
 */
static void zcl_level_attr_value_handler(const ezb_zcl_attribute_t *attribute)
{
    ESP_RETURN_ON_FALSE(attribute, , TAG, "attribute is invalid");
    switch (attribute->id)
    {
    case EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID:
        light_driver_set_brightness(*(uint8_t *)attribute->data.value);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported level attribute ID(0x%04x)", attribute->id);
        break;
    }
}

/**
 * @brief Handle color control cluster attribute changes
 */
static void zcl_color_attr_value_handler(const ezb_zcl_attribute_t *attribute)
{
    static uint16_t cur_color_x = 0;
    static uint16_t cur_color_y = 0;
    static uint16_t new_color_x = 0;
    static uint16_t new_color_y = 0;

    ESP_RETURN_ON_FALSE(attribute, , TAG, "attribute is invalid");

    switch (attribute->id)
    {
    case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID:
        new_color_x = *(uint16_t *)attribute->data.value;
        break;
    case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID:
        new_color_y = *(uint16_t *)attribute->data.value;
        break;
    default:
        ESP_LOGW(TAG, "Unsupported color attribute ID(0x%04x)", attribute->id);
        break;
    }
    if (new_color_x != cur_color_x || new_color_y != cur_color_y)
    {
        light_driver_set_color(new_color_x, new_color_y);
        cur_color_x = new_color_x;
        cur_color_y = new_color_y;
        ESP_LOGI(TAG, "Set Color: x=0x%04x, y=0x%04x", cur_color_x, cur_color_y);
    }
}

/**
 * @brief Handle ZCL attribute set value message
 */
static void apply_motor_direction(motor_direction_t direction)
{
#ifdef CONFIG_PM_ENABLE
    if (direction == MOTOR_DIRECTION_STOP && s_motion_pm_lock_held)
    {
        ESP_ERROR_CHECK(esp_pm_lock_release(pm_lock));
        s_motion_pm_lock_held = false;
    }
    else if (direction != MOTOR_DIRECTION_STOP && !s_motion_pm_lock_held)
    {
        ESP_ERROR_CHECK(esp_pm_lock_acquire(pm_lock));
        s_motion_pm_lock_held = true;
    }
#endif

    switch (direction)
    {
    case MOTOR_DIRECTION_FORWARD:
        motor_forward(255);
        break;
    case MOTOR_DIRECTION_BACKWARD:
        motor_backward(255);
        break;
    default:
        motor_stop();
        break;
    }

    s_motor_direction = direction;
}

static uint8_t clamp_tilt_percentage(int32_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 100)
    {
        return 100;
    }
    return (uint8_t)value;
}

static esp_err_t load_config_state(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        s_full_travel_ms = MOTOR_FULL_TRAVEL_MS;
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    uint32_t travel_ms = MOTOR_FULL_TRAVEL_MS;
    if (nvs_get_u32(handle, CONFIG_KEY_TRAVEL_MS, &travel_ms) == ESP_OK)
    {
        s_full_travel_ms = travel_ms;
    }

    nvs_get_u32(handle, CONFIG_KEY_ENDPOINT_CALIBRATION_MS, &s_endpoint_calibration_ms);

    uint8_t tilt_percentage = 0;
    if (nvs_get_u8(handle, CONFIG_KEY_TILT_PERCENTAGE, &tilt_percentage) == ESP_OK)
    {
        s_tilt_percentage = clamp_tilt_percentage(tilt_percentage);
    }

    if (s_full_travel_ms == 0)
    {
        s_full_travel_ms = MOTOR_FULL_TRAVEL_MS;
    }

    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_config_state(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u32(handle, CONFIG_KEY_TRAVEL_MS, s_full_travel_ms);
    if (err == ESP_OK)
    {
        err = nvs_set_u32(handle, CONFIG_KEY_ENDPOINT_CALIBRATION_MS, s_endpoint_calibration_ms);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, CONFIG_KEY_TILT_PERCENTAGE, s_tilt_percentage);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static uint32_t compute_percent_delta_time_ms(uint8_t target_percentage)
{
    int32_t delta = (int32_t)target_percentage - (int32_t)s_tilt_percentage;
    if (delta < 0)
    {
        delta = -delta;
    }

    return (uint32_t)((delta * s_full_travel_ms) / 100U);
}

static void publish_tilt_percentage(uint8_t tilt_percentage)
{
    ezb_zcl_status_t status;
    ezb_err_t report_status = EZB_ERR_NONE;
    uint8_t reported_tilt_percentage = 100U - tilt_percentage;
    ezb_zcl_report_attr_cmd_t report_attr_cmd = {
        .cmd_ctrl =
            {
                .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
                .src_ep = ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID,
                .cluster_id = EZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
            },
        .payload =
            {
                .attr_id = EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_ID,
            },
    };

    s_tilt_percentage = tilt_percentage;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    status = ezb_zcl_set_attr_value(ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID,
                                    EZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
                                    EZB_ZCL_CLUSTER_SERVER,
                                    EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_ID,
                                    EZB_ZCL_STD_MANUF_CODE,
                                    &reported_tilt_percentage,
                                    false);
    if (status == EZB_ZCL_STATUS_SUCCESS)
    {
        report_status = ezb_zcl_report_attr_cmd_req(&report_attr_cmd);
    }
    esp_zigbee_lock_release();

    if (status != EZB_ZCL_STATUS_SUCCESS)
    {
        ESP_LOGW(TAG, "Failed to update tilt percentage attribute: status(0x%02x)", status);
    }
    else
    {
        ESP_LOGI(TAG, "Tilt percentage updated to %u%% (reported %u%%)",
             s_tilt_percentage, reported_tilt_percentage);
        if (save_config_state() != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to persist tilt percentage to NVS");
        }
        if (report_status != EZB_ERR_NONE)
        {
            ESP_LOGW(TAG, "Failed to send tilt percentage report: error(0x%04x)", report_status);
        }
    }
}

static void apply_travel_time_config(uint32_t travel_ms)
{
    if (travel_ms == 0)
    {
        travel_ms = MOTOR_FULL_TRAVEL_MS;
    }

    s_full_travel_ms = travel_ms;
    ESP_LOGI(TAG, "Updated full travel time to %lu ms", (unsigned long)s_full_travel_ms);

    if (save_config_state() != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to persist full travel time to NVS");
    }
}

static void apply_endpoint_calibration_config(uint32_t calibration_ms)
{
    s_endpoint_calibration_ms = calibration_ms;
    ESP_LOGI(TAG, "Updated endpoint calibration time to %lu ms",
             (unsigned long)s_endpoint_calibration_ms);

    if (save_config_state() != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to persist endpoint calibration time to NVS");
    }
}

static void start_zigbee_move_to_percentage(uint8_t target_percentage)
{
    uint8_t clamped_target = clamp_tilt_percentage((int32_t)target_percentage);
    int32_t delta = (int32_t)clamped_target - (int32_t)s_tilt_percentage;

    if (delta == 0)
    {
        if ((clamped_target == 0 || clamped_target == 100) && s_endpoint_calibration_ms > 0)
        {
            motor_direction_t endpoint_direction = (clamped_target == 100) ?
                MOTOR_DIRECTION_FORWARD : MOTOR_DIRECTION_BACKWARD;
            s_target_tilt_percentage = clamped_target;
            s_motor_control_source = MOTOR_CONTROL_SOURCE_ZIGBEE;
            s_zigbee_move_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(s_endpoint_calibration_ms);
            if (s_button_task_handle != NULL)
            {
                xTaskNotifyGive(s_button_task_handle);
            }
            apply_motor_direction(endpoint_direction);
            ESP_LOGI(TAG, "Calibrating tilt endpoint %u%% for %lu ms",
                     clamped_target, (unsigned long)s_endpoint_calibration_ms);
            return;
        }

        s_motor_control_source = MOTOR_CONTROL_SOURCE_ZIGBEE;
        s_target_tilt_percentage = clamped_target;
        s_zigbee_move_deadline = xTaskGetTickCount();
        if (s_button_task_handle != NULL)
        {
            xTaskNotifyGive(s_button_task_handle);
        }
        apply_motor_direction(MOTOR_DIRECTION_STOP);
        return;
    }

    uint32_t run_ms = compute_percent_delta_time_ms(clamped_target);
    if ((clamped_target == 0 || clamped_target == 100) &&
        s_endpoint_calibration_ms <= UINT32_MAX - run_ms)
    {
        run_ms += s_endpoint_calibration_ms;
    }
    motor_direction_t direction = (delta > 0) ? MOTOR_DIRECTION_FORWARD : MOTOR_DIRECTION_BACKWARD;

    s_target_tilt_percentage = clamped_target;
    s_motor_control_source = MOTOR_CONTROL_SOURCE_ZIGBEE;
    s_zigbee_move_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(run_ms);
    if (s_button_task_handle != NULL)
    {
        xTaskNotifyGive(s_button_task_handle);
    }
    apply_motor_direction(direction);

    ESP_LOGI(TAG, "Moving tilt from %u%% to %u%% for %lu ms (%s) using full-travel %lu ms and endpoint calibration %lu ms",
             s_tilt_percentage, clamped_target, (unsigned long)run_ms,
             direction == MOTOR_DIRECTION_FORWARD ? "forward" : "backward",
             (unsigned long)s_full_travel_ms,
             (unsigned long)s_endpoint_calibration_ms);
}

static void zcl_window_covering_attr_value_handler(const ezb_zcl_attribute_t *attribute)
{
    ESP_RETURN_ON_FALSE(attribute, , TAG, "attribute is invalid");

    switch (attribute->id)
    {
    case EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_ID:
        s_tilt_percentage = 100U - *(uint8_t *)attribute->data.value;
        break;
    case EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_ID:
        s_tilt_percentage = 100U - clamp_tilt_percentage((int32_t)(*(uint16_t *)attribute->data.value));
        break;
    default:
        ESP_LOGW(TAG, "Unsupported window covering attribute ID(0x%04x)", attribute->id);
        return;
    }

    start_zigbee_move_to_percentage(s_tilt_percentage);
    ESP_LOGI(TAG, "Window covering attribute updated to %u%%", s_tilt_percentage);
}

static void zcl_core_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    ESP_LOGI(TAG, "ZCL SetAttributeValue for endpoint(%d) cluster(0x%04x)",
             message->info.dst_ep, message->info.cluster_id);

    switch (message->info.cluster_id)
    {
    case EZB_ZCL_CLUSTER_ID_ON_OFF:
        zcl_on_off_attr_value_handler(&message->in.attribute);
        break;
    case EZB_ZCL_CLUSTER_ID_LEVEL:
        zcl_level_attr_value_handler(&message->in.attribute);
        break;
    case EZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
        zcl_color_attr_value_handler(&message->in.attribute);
        break;
    case EZB_ZCL_CLUSTER_ID_WINDOW_COVERING:
        if (message->in.attribute.id == WINDOW_COVERING_TRAVEL_TIME_ATTR_ID &&
            message->in.attribute.data.type == EZB_ZCL_ATTR_TYPE_UINT32 &&
            message->in.attribute.data.size == sizeof(uint32_t))
        {
            uint32_t new_travel_ms = *(uint32_t *)message->in.attribute.data.value;
            apply_travel_time_config(new_travel_ms);
            break;
        }
        if (message->in.attribute.id == WINDOW_COVERING_ENDPOINT_CALIBRATION_ATTR_ID &&
            message->in.attribute.data.type == EZB_ZCL_ATTR_TYPE_UINT32 &&
            message->in.attribute.data.size == sizeof(uint32_t))
        {
            uint32_t new_calibration_ms = *(uint32_t *)message->in.attribute.data.value;
            apply_endpoint_calibration_config(new_calibration_ms);
            break;
        }
        zcl_window_covering_attr_value_handler(&message->in.attribute);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported cluster ID(0x%04x)", message->info.cluster_id);
    }
}

/**
 * @brief ZCL core action handler
 */
static void handle_window_covering_movement(ezb_zcl_window_covering_movement_message_t *message)
{
    if (!message)
    {
        return;
    }

    if (!message->in.header)
    {
        return;
    }

    switch (message->in.header->cmd_id)
    {
    case EZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN_ID:
        start_zigbee_move_to_percentage(100);
        ESP_LOGI(TAG, "Window covering state open mapped to tilt 100%%");
        break;
    case EZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE_ID:
        start_zigbee_move_to_percentage(0);
        ESP_LOGI(TAG, "Window covering state close mapped to tilt 0%%");
        break;
    case EZB_ZCL_CMD_WINDOW_COVERING_STOP_ID:
        s_zigbee_move_deadline = 0;
        s_motor_control_source = MOTOR_CONTROL_SOURCE_NONE;
        apply_motor_direction(MOTOR_DIRECTION_STOP);
        ESP_LOGI(TAG, "Window covering state stop mapped to tilt stop");
        break;
    case EZB_ZCL_CMD_WINDOW_COVERING_GO_TO_TILT_PERCENTAGE_ID:
        if (message->in.payload.tilt_percentage <= 0x64)
        {
            uint8_t internal_target = 100U - message->in.payload.tilt_percentage;
            start_zigbee_move_to_percentage(internal_target);
            ESP_LOGI(TAG, "Window covering tilt percentage set to %u (internal %u)",
                     message->in.payload.tilt_percentage, internal_target);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported window covering command ID(0x%02x)", message->in.header->cmd_id);
        break;
    }
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id)
    {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        zcl_core_set_attr_value_handler(message);
        break;
    case EZB_ZCL_CORE_WINDOW_COVERING_MOVEMENT_CB_ID:
    {
        ezb_zcl_window_covering_movement_message_t *movement_msg =
            (ezb_zcl_window_covering_movement_message_t *)message;
        handle_window_covering_movement(movement_msg);
    }
    break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID:
    {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGI(TAG, "Received ZCL Default Response with status(0x%02x)", default_rsp->in.status_code);
    }
    break;
    default:
        ESP_LOGD(TAG, "ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

static void battery_update_handler(uint8_t battery_percentage)
{
    static uint8_t last_battery_percentage = UINT8_MAX;
    ezb_zcl_status_t status;
    ezb_err_t report_status = EZB_ERR_NONE;
    ezb_zcl_report_attr_cmd_t report_attr_cmd = {
        .cmd_ctrl =
            {
                .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
                .src_ep = ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID,
                .cluster_id = EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            },
        .payload =
            {
                .attr_id = EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            },
    };

    if (battery_percentage == last_battery_percentage)
    {
        return;
    }

    ESP_LOGI(TAG, "Battery update: percentage=%d%%", battery_percentage / 2);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    status = ezb_zcl_set_attr_value(
        ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID,
        EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        EZB_ZCL_STD_MANUF_CODE,
        (uint8_t *)&battery_percentage,
        false);
    if (status == EZB_ZCL_STATUS_SUCCESS)
    {
        report_status = ezb_zcl_report_attr_cmd_req(&report_attr_cmd);
    }
    esp_zigbee_lock_release();

    if (status != EZB_ZCL_STATUS_SUCCESS)
    {
        ESP_LOGW(TAG, "Failed to update battery attribute: status(0x%02x)", status);
    }
    else if (report_status != EZB_ERR_NONE)
    {
        ESP_LOGW(TAG, "Failed to send battery report: error(0x%04x)", report_status);
    }
    else
    {
        last_battery_percentage = battery_percentage;
    }
}

static void buttons_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_FORWARD_GPIO) | (1ULL << BUTTON_BACKWARD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_FORWARD_GPIO, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_BACKWARD_GPIO, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(
        (1ULL << BUTTON_FORWARD_GPIO) | (1ULL << BUTTON_BACKWARD_GPIO),
        ESP_EXT1_WAKEUP_ANY_LOW));

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
    rtc_gpio_pulldown_dis(BUTTON_FORWARD_GPIO);
    rtc_gpio_pullup_en(BUTTON_FORWARD_GPIO);
    rtc_gpio_pulldown_dis(BUTTON_BACKWARD_GPIO);
    rtc_gpio_pullup_en(BUTTON_BACKWARD_GPIO);
#else
    gpio_pulldown_dis(BUTTON_FORWARD_GPIO);
    gpio_pullup_en(BUTTON_FORWARD_GPIO);
    gpio_pulldown_dis(BUTTON_BACKWARD_GPIO);
    gpio_pullup_en(BUTTON_BACKWARD_GPIO);
#endif
    ESP_LOGI(TAG, "Button inputs initialized on GPIO %d and %d", BUTTON_FORWARD_GPIO, BUTTON_BACKWARD_GPIO);
}

static void IRAM_ATTR button_gpio_isr(void *arg)
{
    gpio_num_t gpio_num = (gpio_num_t)(uintptr_t)arg;
    gpio_intr_disable(gpio_num);

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_button_task_handle != NULL)
    {
        vTaskNotifyGiveFromISR(s_button_task_handle, &higher_priority_task_woken);
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void update_button_tilt(motor_direction_t direction, int64_t start_time_us, uint8_t start_tilt)
{
    int64_t elapsed_time_us = esp_timer_get_time() - start_time_us;
    if (elapsed_time_us < 0)
    {
        elapsed_time_us = 0;
    }

    uint32_t delta = (uint32_t)(((uint64_t)elapsed_time_us * 100U) /
                                ((uint64_t)s_full_travel_ms * 1000U));
    int32_t target = start_tilt;

    if (direction == MOTOR_DIRECTION_FORWARD)
    {
        target += (int32_t)delta;
    }
    else
    {
        target -= (int32_t)delta;
    }

    uint8_t target_tilt = clamp_tilt_percentage(target);
    s_tilt_percentage = target_tilt;
}

static void button_control_task(void *arg)
{
    (void)arg;

    bool forward_pressed = false;
    bool backward_pressed = false;
    bool candidate_forward = false;
    bool candidate_backward = false;
    int64_t button_move_start_us = 0;
    uint8_t button_move_start_tilt = 0;
    motor_direction_t button_move_direction = MOTOR_DIRECTION_STOP;
    TickType_t last_state_change = xTaskGetTickCount();

    while (1)
    {
        bool new_forward = (gpio_get_level(BUTTON_FORWARD_GPIO) == BUTTON_ACTIVE_LEVEL);
        bool new_backward = (gpio_get_level(BUTTON_BACKWARD_GPIO) == BUTTON_ACTIVE_LEVEL);
        bool raw_button_activity = new_forward || new_backward;

        TickType_t now = xTaskGetTickCount();
        if (new_forward != candidate_forward || new_backward != candidate_backward)
        {
            candidate_forward = new_forward;
            candidate_backward = new_backward;
            last_state_change = now;
        }

        if ((candidate_forward != forward_pressed || candidate_backward != backward_pressed) &&
            (now - last_state_change) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS))
        {
            forward_pressed = candidate_forward;
            backward_pressed = candidate_backward;
        }

        bool button_activity = forward_pressed || backward_pressed;
        bool button_state_pending = raw_button_activity != button_activity;

        if (s_motor_control_source == MOTOR_CONTROL_SOURCE_ZIGBEE &&
            !button_activity && !raw_button_activity)
        {
            if (s_zigbee_move_deadline != 0 && xTaskGetTickCount() >= s_zigbee_move_deadline)
            {
                publish_tilt_percentage(s_target_tilt_percentage);
                s_zigbee_move_deadline = 0;
                s_motor_control_source = MOTOR_CONTROL_SOURCE_NONE;
                apply_motor_direction(MOTOR_DIRECTION_STOP);
            }
            else
            {
                apply_motor_direction(s_motor_direction);
            }
        }
        else if (forward_pressed && !backward_pressed)
        {
            if (s_motor_control_source != MOTOR_CONTROL_SOURCE_BUTTONS ||
                s_motor_direction != MOTOR_DIRECTION_FORWARD)
            {
                button_move_start_us = esp_timer_get_time();
                button_move_start_tilt = s_tilt_percentage;
                button_move_direction = MOTOR_DIRECTION_FORWARD;
            }
            s_motor_control_source = MOTOR_CONTROL_SOURCE_BUTTONS;
            s_zigbee_move_deadline = 0;
            update_button_tilt(MOTOR_DIRECTION_FORWARD, button_move_start_us, button_move_start_tilt);
            apply_motor_direction(MOTOR_DIRECTION_FORWARD);
        }
        else if (backward_pressed && !forward_pressed)
        {
            if (s_motor_control_source != MOTOR_CONTROL_SOURCE_BUTTONS ||
                s_motor_direction != MOTOR_DIRECTION_BACKWARD)
            {
                button_move_start_us = esp_timer_get_time();
                button_move_start_tilt = s_tilt_percentage;
                button_move_direction = MOTOR_DIRECTION_BACKWARD;
            }
            s_motor_control_source = MOTOR_CONTROL_SOURCE_BUTTONS;
            s_zigbee_move_deadline = 0;
            update_button_tilt(MOTOR_DIRECTION_BACKWARD, button_move_start_us, button_move_start_tilt);
            apply_motor_direction(MOTOR_DIRECTION_BACKWARD);
        }
        else
        {
            if (s_motor_control_source == MOTOR_CONTROL_SOURCE_BUTTONS &&
                !button_activity && !button_state_pending)
            {
                update_button_tilt(button_move_direction, button_move_start_us, button_move_start_tilt);
                publish_tilt_percentage(s_tilt_percentage);
                s_motor_control_source = MOTOR_CONTROL_SOURCE_NONE;
                s_zigbee_move_deadline = 0;
            }
            apply_motor_direction(MOTOR_DIRECTION_STOP);
        }

        if (!raw_button_activity)
        {
            gpio_intr_enable(BUTTON_FORWARD_GPIO);
            gpio_intr_enable(BUTTON_BACKWARD_GPIO);
        }

        if (!button_activity && !button_state_pending &&
            s_motor_control_source == MOTOR_CONTROL_SOURCE_NONE)
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;

    ESP_RETURN_ON_FALSE(!is_inited, ESP_OK, TAG, "Deferred driver already initialized");

    battery_driver_init(ESP_BATTERY_ATTR_UPDATE_INTERVAL, battery_update_handler);

    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pm_lock", &pm_lock));
    is_inited = true;

    motor_init();
    buttons_init();
    if (xTaskCreate(button_control_task, "button_control", 4096, NULL, 5, &s_button_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create button control task");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_FORWARD_GPIO, button_gpio_isr,
                                         (void *)(uintptr_t)BUTTON_FORWARD_GPIO));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_BACKWARD_GPIO, button_gpio_isr,
                                         (void *)(uintptr_t)BUTTON_BACKWARD_GPIO));

    return is_inited ? ESP_OK : ESP_FAIL;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

/**
 * @brief Signal handler for Zigbee events
 */
static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type)
    {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        light_driver_set_zigbee_connecting(true);
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
    {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new())
            {
                light_driver_set_zigbee_connecting(true);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
            else
            {
                light_driver_set_zigbee_connecting(false);
                ESP_LOGI(TAG, "Device reboot");
            }
        }
        else
        {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), please retry", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    }
    break;
    case EZB_BDB_SIGNAL_STEERING:
    {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS)
        {
            light_driver_set_zigbee_connecting(false);
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
        }
        else
        {
            ESP_LOGW(TAG, "Failed to join network with status(0x%02x)", status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    }
    break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE:
    {
        const ezb_zdo_signal_device_annce_params_t *dev_annce_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->short_addr);
    }
    break;
    case EZB_ZDO_SIGNAL_LEAVE:
    {
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network successfully with type(0x%02x)", leave_params->leave_type);
    }
    break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
    {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        if (duration)
        {
            ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", ezb_nwk_get_panid(), duration);
        }
        else
        {
            ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", ezb_nwk_get_panid());
        }
    }
    break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

/**
 * @brief Create a Zigbee color dimmable light device
 */
static esp_err_t esp_zigbee_create_light_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_window_covering_config_t window_cfg = EZB_ZHA_WINDOW_COVERING_CONFIG();
    window_cfg.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_window_covering(ESP_ZIGBEE_HA_COLOR_DIMMABLE_LIGHT_EP_ID, &window_cfg);
    ezb_zcl_cluster_desc_t basic_desc = {0};

    basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ESP_MODEL_IDENTIFIER);

    static uint8_t battery_percentage = 0;
    ezb_zcl_cluster_desc_t power_desc = ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_power_config_cluster_desc_add_attr(power_desc, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                               &battery_percentage);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, power_desc));

    static uint16_t current_tilt = 0;
    ezb_zcl_cluster_desc_t window_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_WINDOW_COVERING, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_window_covering_cluster_desc_add_attr(window_desc, EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_ID, &current_tilt);
    ezb_zcl_window_covering_cluster_desc_add_attr(window_desc, EZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_ID, &s_tilt_percentage);
    ezb_zcl_cluster_desc_add_manuf_attr(window_desc, WINDOW_COVERING_TRAVEL_TIME_ATTR_ID,
                                       EZB_ZCL_ATTR_TYPE_UINT32,
                                       EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE,
                                       WINDOW_COVERING_TRAVEL_TIME_MANUF_CODE,
                                       &s_full_travel_ms);
    ezb_zcl_cluster_desc_add_manuf_attr(window_desc, WINDOW_COVERING_ENDPOINT_CALIBRATION_ATTR_ID,
                                       EZB_ZCL_ATTR_TYPE_UINT32,
                                       EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE,
                                       WINDOW_COVERING_TRAVEL_TIME_MANUF_CODE,
                                       &s_endpoint_calibration_ms);

    ezb_af_node_power_desc_t node_power_desc = {
        .current_power_mode = EZB_AF_NODE_POWER_MODE_COME_ON_PERIODICALLY,
        .available_power_sources = EZB_AF_NODE_POWER_SOURCE_RECHARGEABLE_BATTERY,
        .current_power_source = EZB_AF_NODE_POWER_SOURCE_RECHARGEABLE_BATTERY,
        .current_power_source_level = EZB_AF_NODE_POWER_SOURCE_LEVEL_100_PERCENT,
    };
    ESP_ERROR_CHECK(ezb_af_set_node_power_desc(&node_power_desc));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

/**
 * @brief Setup Zigbee commissioning parameters
 */
static esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    /* Keep RX on when idle to receive commands */
    ezb_nwk_set_rx_on_when_idle(false);

    return ESP_OK;
}

#ifdef CONFIG_PM_ENABLE

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t esp_pm_entry_light_sleep_cb(int64_t sleep_time_us, void *arg)
{
    ESP_EARLY_LOGI(TAG, "Enter Light Sleep");
    return ESP_OK;
}

esp_err_t esp_pm_exit_light_sleep_cb(int64_t sleep_time_us, void *arg)
{
    ESP_EARLY_LOGI(TAG, "Exit Light Sleep");
    return ESP_OK;
}

esp_pm_sleep_cbs_register_config_t s_sleep_cbs_config = {
    .enter_cb = esp_pm_entry_light_sleep_cb,
    .exit_cb = esp_pm_exit_light_sleep_cb,
    .enter_cb_user_arg = NULL,
    .exit_cb_user_arg = NULL,
    .enter_cb_prior = 0,
    .exit_cb_prior = 0,
};
#endif /* CONFIG_PM_LIGHT_SLEEP_CALLBACKS */

static esp_err_t esp_pm_light_sleep_config(void)
{
    esp_err_t rc = ESP_OK;
    // Keep clocks required by the Zigbee/light-sleep stack powered.
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_ON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON);

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    int cur_cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t pm_config = {
        .max_freq_mhz = cur_cpu_freq_mhz,
        .min_freq_mhz = cur_cpu_freq_mhz,
        .light_sleep_enable = true,
    };
    rc = esp_pm_configure(&pm_config);
#endif /* CONFIG_FREERTOS_USE_TICKLESS_IDLE */

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    rc == ESP_OK ? esp_pm_light_sleep_register_cbs(&s_sleep_cbs_config) : rc;
#endif /* CONFIG_PM_LIGHT_SLEEP_CALLBACKS */

    return rc;
}
#endif /* CONFIG_PM_ENABLE */

/**
 * @brief Main Zigbee stack task
 */
static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());
    ESP_ERROR_CHECK(esp_zigbee_create_light_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    ezb_nwk_set_min_join_lqi(0);

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/**
 * @brief Application main entry point
 */
void app_main(void)
{
    // Force motor driver inputs to a known-safe state ASAP
    gpio_config_t motor_safe_conf = {
        .pin_bit_mask = (1ULL << MOTOR_FORWARD_GPIO) | (1ULL << MOTOR_BACKWARD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&motor_safe_conf);
    gpio_set_level(MOTOR_FORWARD_GPIO, 0);
    gpio_set_level(MOTOR_BACKWARD_GPIO, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME));
    ESP_ERROR_CHECK(load_config_state());

    /* Initialize LED */
    ESP_ERROR_CHECK(light_driver_init());

#ifdef CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_light_sleep_config());
#endif

    ESP_LOGI(TAG, "Start ESP Zigbee Stack");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
