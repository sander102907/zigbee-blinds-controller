#include "esp_adc/adc_oneshot.h"
#include "ezbee/zha.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "ezbee/zcl/cluster/power_config.h"
#include "battery_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "driver/gpio.h"

#define BAT_ADC CONFIG_BATTERY_MEASUREMENT_GPIO // For example GPIO0, adjust as needed
#define ADC_ATTEN ADC_ATTEN_DB_12

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t battery_pm_lock;
#endif

bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);

adc_channel_t channel;

adc_cali_handle_t adc_cali_handle = NULL;
bool do_calibration = false;

/* update interval in seconds */
static uint16_t interval = 5;

/* callback function pointer */
static esp_battery_callback_t battery_cb;

void battery_driver_init_adc()
{
    adc_unit_t unit;

    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BAT_ADC, &unit, &channel));

    do_calibration = adc_calibration_init(unit, channel, ADC_ATTEN, &adc_cali_handle);
}

/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGI("BATTERY", "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGI("BATTERY", "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK)
    {
        ESP_LOGI("BATTERY", "Calibration Success");
    }
    else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated)
    {
        ESP_LOGW("BATTERY", "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE("BATTERY", "Invalid arg or no memory");
    }

    return calibrated;
}

int get_battery_milli_volts()
{
    esp_pm_lock_acquire(battery_pm_lock);

    static adc_oneshot_unit_handle_t adc1_handle = NULL;

    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, channel, &config));

    // Discard first conversion - known ESP32 ADC quirk, unrelated to settle time
    int dummy;
    adc_oneshot_read(adc1_handle, channel, &dummy);

    int num_samples = 16;
    int adc_raw_sum = 0;

    for (int i = 0; i < num_samples; i++)
    {
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, channel, &adc_raw));
        adc_raw_sum += adc_raw;
    }

    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
    adc1_handle = NULL;

    esp_pm_lock_release(battery_pm_lock);

    int adc_raw_avg = adc_raw_sum / num_samples;

    if (do_calibration)
    {
        int voltage_mv;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_raw_avg, &voltage_mv));
        return voltage_mv * 2; // apply divider factor AFTER calibration
    }

    return adc_raw_avg * 2; // uncalibrated fallback, still raw-ish
}

int calculate_battery_percentage(int voltage_mv)
{
    double v = voltage_mv / 1000.0; // Convert mV to V

    // This formula is based on LiPo discharge characteristics
    double percentage = -144.9390 * v * v * v + 1655.8629 * v * v - 6158.8520 * v + 7501.3202;

    // Clamp the value between 0 and 100
    if (percentage > 100)
        percentage = 100;
    if (percentage < 0)
        percentage = 0;

    return (int)(percentage);
}

// Function to update the battery attributes in the Zigbee cluster
void update_battery_attributes()
{
    for (;;)
    {
        int battery_mv = get_battery_milli_volts();                        // Get battery voltage in mV
        int battery_percentage = calculate_battery_percentage(battery_mv); // Get battery % (0-100)

        ESP_LOGI("BATTERY", "Measured battery voltage: %d mV, percentage: %d%%",
             battery_mv, battery_percentage);

        // Convert values to Zigbee format
        uint8_t zigbee_battery_percentage = (battery_percentage * 2); // Zigbee uses 0-200 range

        if (battery_cb)
        {
            battery_cb(zigbee_battery_percentage);
        }

        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}

static esp_err_t battery_init()
{
    return (xTaskCreate(update_battery_attributes, "battery_update", 4096, NULL, 10, NULL) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t battery_driver_init(uint16_t update_interval, esp_battery_callback_t cb)
{
    battery_driver_init_adc();

    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "battery_adc", &battery_pm_lock));

    interval = update_interval;
    battery_cb = cb;
    return battery_init();
}