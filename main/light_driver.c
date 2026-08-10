#include "light_driver.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_GPIO 8
#define LED_OFF 0
#define LED_ON 1

static const char *TAG = "LIGHT_DRIVER";
static led_strip_handle_t s_led_strip;
static volatile bool s_zigbee_connecting;
static TaskHandle_t s_connection_led_task;

static void light_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

static void zigbee_connection_led_task(void *arg)
{
    (void)arg;

    while (true)
    {
        if (s_zigbee_connecting)
        {
            light_driver_set_rgb(0, 0, 255);
            vTaskDelay(pdMS_TO_TICKS(500));
            light_driver_set_rgb(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

esp_err_t light_driver_init(void)
{
    led_strip_config_t led_strip_conf = {
        .max_leds = 1,
        .strip_gpio_num = LED_GPIO,
    };
    led_strip_rmt_config_t rmt_conf = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_strip_conf, &rmt_conf, &s_led_strip));
    ESP_ERROR_CHECK(xTaskCreate(zigbee_connection_led_task, "zigbee_conn_led", 2048, NULL, 2,
                                &s_connection_led_task) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "LED initialized on GPIO %d", LED_GPIO);
    return ESP_OK;
}

void light_driver_set_state(uint8_t state)
{
    bool power = (state != 0);

    light_driver_set_rgb(255 * power, 255 * power, 255 * power);
    ESP_LOGI(TAG, "LED state set to: %s", state ? "ON" : "OFF");
}

void light_driver_set_brightness(uint8_t level)
{
    gpio_set_level(LED_GPIO, level > 0 ? LED_ON : LED_OFF);
    ESP_LOGI(TAG, "LED brightness set to: %d", level);
}

void light_driver_set_color(uint16_t color_x, uint16_t color_y)
{
    ESP_LOGI(TAG, "Set Color: x=0x%04x, y=0x%04x", color_x, color_y);
}

void light_driver_set_zigbee_connecting(bool connecting)
{
    s_zigbee_connecting = connecting;
    if (s_connection_led_task != NULL)
    {
        xTaskNotifyGive(s_connection_led_task);
    }
}
