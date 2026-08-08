#include "driver/ledc.h"

#define MOTOR_FORWARD_GPIO CONFIG_MOTOR_FORWARD_GPIO
#define MOTOR_BACKWARD_GPIO CONFIG_MOTOR_BACKWARD_GPIO
#define MOTOR_FORWARD_CHANNEL LEDC_CHANNEL_0
#define MOTOR_BACKWARD_CHANNEL LEDC_CHANNEL_1
#define PWM_FREQ 1000    // 1kHz frequency
#define PWM_RESOLUTION 8 // 8-bit (0-255)

// Initialize PWM for motor control
static void motor_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz = PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf_forward = {
        .gpio_num = MOTOR_FORWARD_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = MOTOR_FORWARD_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0, // Start off
        .hpoint = 0};
    ledc_channel_config(&channel_conf_forward);

    ledc_channel_config_t channel_conf_backward = {
        .gpio_num = MOTOR_BACKWARD_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = MOTOR_BACKWARD_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0, // Start off
        .hpoint = 0};
    ledc_channel_config(&channel_conf_backward);
}

// Set motor power (0-255)
static void motor_forward(uint8_t power)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL, power);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL);
}

// Set motor power (0-255)
static void motor_backward(uint8_t power)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL, power);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL);
}

// Stop motor
static void motor_stop()
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_BACKWARD_CHANNEL);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_FORWARD_CHANNEL);
}